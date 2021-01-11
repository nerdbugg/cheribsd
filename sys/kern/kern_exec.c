/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1993, David Greenman
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_capsicum.h"
#include "opt_hwpmc_hooks.h"
#include "opt_ktrace.h"
#include "opt_vm.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/acct.h>
#include <sys/capsicum.h>
#include <sys/eventhandler.h>
#include <sys/exec.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/imgact.h>
#include <sys/imgact_elf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/resourcevar.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/sdt.h>
#include <sys/sf_buf.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/smp.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/timers.h>
#include <sys/umtx.h>
#include <sys/vnode.h>
#include <sys/wait.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_pager.h>

#ifdef	HWPMC_HOOKS
#include <sys/pmckern.h>
#endif

#include <machine/reg.h>

#include <security/audit/audit.h>
#include <security/mac/mac_framework.h>

#if __has_feature(capabilities)
#include <cheri/cheri.h>
#include <cheri/cheric.h>
#include <cheri/cherireg.h>
#endif

#ifdef KDTRACE_HOOKS
#include <sys/dtrace_bsd.h>
dtrace_execexit_func_t	dtrace_fasttrap_exec;
#endif

SDT_PROVIDER_DECLARE(proc);
SDT_PROBE_DEFINE1(proc, , , exec, "char *");
SDT_PROBE_DEFINE1(proc, , , exec__failure, "int");
SDT_PROBE_DEFINE1(proc, , , exec__success, "char *");

MALLOC_DEFINE(M_PARGS, "proc-args", "Process arguments");

int coredump_pack_fileinfo = 1;
SYSCTL_INT(_kern, OID_AUTO, coredump_pack_fileinfo, CTLFLAG_RWTUN,
    &coredump_pack_fileinfo, 0,
    "Enable file path packing in 'procstat -f' coredump notes");

int coredump_pack_vmmapinfo = 1;
SYSCTL_INT(_kern, OID_AUTO, coredump_pack_vmmapinfo, CTLFLAG_RWTUN,
    &coredump_pack_vmmapinfo, 0,
    "Enable file path packing in 'procstat -v' coredump notes");

static int sysctl_kern_ps_strings(SYSCTL_HANDLER_ARGS);
static int sysctl_kern_usrstack(SYSCTL_HANDLER_ARGS);
static int sysctl_kern_stackprot(SYSCTL_HANDLER_ARGS);
static int do_execve(struct thread *td, struct image_args *args,
    void * __capability mac_p, struct vmspace *oldvmspace);

/* XXX This should be vm_size_t. */
SYSCTL_PROC(_kern, KERN_PS_STRINGS, ps_strings, CTLTYPE_ULONG|CTLFLAG_RD|
    CTLFLAG_CAPRD|CTLFLAG_MPSAFE, NULL, 0, sysctl_kern_ps_strings, "LU",
    "Location of process' ps_strings structure");

/* XXX This should be vm_size_t. */
SYSCTL_PROC(_kern, KERN_USRSTACK, usrstack, CTLTYPE_ULONG|CTLFLAG_RD|
    CTLFLAG_CAPRD|CTLFLAG_MPSAFE, NULL, 0, sysctl_kern_usrstack, "LU",
    "Top of process stack");

SYSCTL_PROC(_kern, OID_AUTO, stackprot, CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_MPSAFE,
    NULL, 0, sysctl_kern_stackprot, "I",
    "Stack memory permissions");

u_long ps_arg_cache_limit = PAGE_SIZE / 16;
SYSCTL_ULONG(_kern, OID_AUTO, ps_arg_cache_limit, CTLFLAG_RW, 
    &ps_arg_cache_limit, 0,
    "Process' command line characters cache limit");

static int disallow_high_osrel;
SYSCTL_INT(_kern, OID_AUTO, disallow_high_osrel, CTLFLAG_RW,
    &disallow_high_osrel, 0,
    "Disallow execution of binaries built for higher version of the world");

static int map_at_zero = 0;
SYSCTL_INT(_security_bsd, OID_AUTO, map_at_zero, CTLFLAG_RWTUN, &map_at_zero, 0,
    "Permit processes to map an object at virtual address 0.");

static int
sysctl_kern_ps_strings(SYSCTL_HANDLER_ARGS)
{
	struct proc *p;
	int error;

	p = curproc;
#ifdef SCTL_MASK32
	if (req->flags & SCTL_MASK32) {
		unsigned int val;
		val = (unsigned int)p->p_psstrings;
		error = SYSCTL_OUT(req, &val, sizeof(val));
	} else
#endif
		error = SYSCTL_OUT(req, &p->p_psstrings,
		   sizeof(p->p_psstrings));
	return error;
}

static int
sysctl_kern_usrstack(SYSCTL_HANDLER_ARGS)
{
	struct proc *p;
	int error;

	p = curproc;
#ifdef SCTL_MASK32
	if (req->flags & SCTL_MASK32) {
		unsigned int val;
		val = (unsigned int)p->p_usrstack;
		error = SYSCTL_OUT(req, &val, sizeof(val));
	} else
#endif
		error = SYSCTL_OUT(req, &p->p_usrstack,
		    sizeof(p->p_usrstack));
	return error;
}

static int
sysctl_kern_stackprot(SYSCTL_HANDLER_ARGS)
{
	struct proc *p;
	int stackprot;

	p = curproc;
	stackprot = p->p_sysent->sv_stackprot & VM_PROT_RWX;
	return (SYSCTL_OUT(req, &stackprot, sizeof(stackprot)));
}

/*
 * Each of the items is a pointer to a `const struct execsw', hence the
 * double pointer here.
 */
static const struct execsw **execsw;

#ifndef _SYS_SYSPROTO_H_
struct execve_args {
	char    *fname; 
	char    **argv;
	char    **envv; 
};
#endif

int
sys_execve(struct thread *td, struct execve_args *uap)
{
	struct image_args args;
	struct vmspace *oldvmspace;
	int error;

	error = pre_execve(td, &oldvmspace);
	if (error != 0)
		return (error);
	error = exec_copyin_args(&args, uap->fname, UIO_USERSPACE,
	    uap->argv, uap->envv);
	if (error == 0)
		error = kern_execve(td, &args, NULL, oldvmspace);
	post_execve(td, error, oldvmspace);
	AUDIT_SYSCALL_EXIT(error == EJUSTRETURN ? 0 : error, td);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct fexecve_args {
	int	fd;
	char	**argv;
	char	**envv;
};
#endif
int
sys_fexecve(struct thread *td, struct fexecve_args *uap)
{
	struct image_args args;
	struct vmspace *oldvmspace;
	int error;

	error = pre_execve(td, &oldvmspace);
	if (error != 0)
		return (error);
	error = exec_copyin_args(&args, NULL, UIO_SYSSPACE,
	    uap->argv, uap->envv);
	if (error == 0) {
		args.fd = uap->fd;
		error = kern_execve(td, &args, NULL, oldvmspace);
	}
	post_execve(td, error, oldvmspace);
	AUDIT_SYSCALL_EXIT(error == EJUSTRETURN ? 0 : error, td);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct __mac_execve_args {
	char	*fname;
	char	**argv;
	char	**envv;
	struct mac	*mac_p;
};
#endif

int
sys___mac_execve(struct thread *td, struct __mac_execve_args *uap)
{
#ifdef MAC
	struct image_args args;
	struct vmspace *oldvmspace;
	int error;

	error = pre_execve(td, &oldvmspace);
	if (error != 0)
		return (error);
	error = exec_copyin_args(&args, uap->fname, UIO_USERSPACE,
	    uap->argv, uap->envv);
	if (error == 0)
		error = kern_execve(td, &args, uap->mac_p, oldvmspace);
	post_execve(td, error, oldvmspace);
	AUDIT_SYSCALL_EXIT(error == EJUSTRETURN ? 0 : error, td);
	return (error);
#else
	return (ENOSYS);
#endif
}

int
pre_execve(struct thread *td, struct vmspace **oldvmspace)
{
	struct proc *p;
	int error;

	KASSERT(td == curthread, ("non-current thread %p", td));
	error = 0;
	p = td->td_proc;
	if ((p->p_flag & P_HADTHREADS) != 0) {
		PROC_LOCK(p);
		if (thread_single(p, SINGLE_BOUNDARY) != 0)
			error = ERESTART;
		PROC_UNLOCK(p);
	}
	KASSERT(error != 0 || (td->td_pflags & TDP_EXECVMSPC) == 0,
	    ("nested execve"));
	*oldvmspace = p->p_vmspace;
	return (error);
}

void
post_execve(struct thread *td, int error, struct vmspace *oldvmspace)
{
	struct proc *p;

	KASSERT(td == curthread, ("non-current thread %p", td));
	p = td->td_proc;
	if ((p->p_flag & P_HADTHREADS) != 0) {
		PROC_LOCK(p);
		/*
		 * If success, we upgrade to SINGLE_EXIT state to
		 * force other threads to suicide.
		 */
		if (error == EJUSTRETURN)
			thread_single(p, SINGLE_EXIT);
		else
			thread_single_end(p, SINGLE_BOUNDARY);
		PROC_UNLOCK(p);
	}
	exec_cleanup(td, oldvmspace);
}

/*
 * kern_execve() has the astonishing property of not always returning to
 * the caller.  If sufficiently bad things happen during the call to
 * do_execve(), it can end up calling exit1(); as a result, callers must
 * avoid doing anything which they might need to undo (e.g., allocating
 * memory).
 */
int
kern_execve(struct thread *td, struct image_args *args,
    void * __capability mac_p, struct vmspace *oldvmspace)
{

	AUDIT_ARG_ARGV(args->begin_argv, args->argc,
	    exec_args_get_begin_envv(args) - args->begin_argv);
	AUDIT_ARG_ENVV(exec_args_get_begin_envv(args), args->envc,
	    args->endp - exec_args_get_begin_envv(args));
	return (do_execve(td, args, mac_p, oldvmspace));
}

/*
 * In-kernel implementation of execve().  All arguments are assumed to be
 * userspace pointers from the passed thread.
 */
static int
do_execve(struct thread *td, struct image_args *args,
    void * __capability umac, struct vmspace *oldvmspace)
{
	struct proc *p = td->td_proc;
	struct nameidata nd;
	struct ucred *oldcred;
	struct uidinfo *euip = NULL;
	uintcap_t stack_base;
	struct image_params image_params, *imgp;
	struct vattr attr;
	int (*img_first)(struct image_params *);
	struct pargs *oldargs = NULL, *newargs = NULL;
	struct sigacts *oldsigacts = NULL, *newsigacts = NULL;
#ifdef KTRACE
	struct vnode *tracevp = NULL;
	struct ucred *tracecred = NULL;
#endif
	struct vnode *oldtextvp = NULL, *newtextvp;
	int credential_changing;
#ifdef MAC
	struct mac extmac;
	struct mac *mac_p;
	struct label *interpvplabel = NULL;
	int will_transition;
#endif
#ifdef HWPMC_HOOKS
	struct pmckern_procexec pe;
#endif
	int error, i, orig_osrel;
	uint32_t orig_fctl0;
	static const char fexecv_proc_title[] = "(fexecv)";

	imgp = &image_params;

	/*
	 * Lock the process and set the P_INEXEC flag to indicate that
	 * it should be left alone until we're done here.  This is
	 * necessary to avoid race conditions - e.g. in ptrace() -
	 * that might allow a local user to illicitly obtain elevated
	 * privileges.
	 */
	PROC_LOCK(p);
	KASSERT((p->p_flag & P_INEXEC) == 0,
	    ("%s(): process already has P_INEXEC flag", __func__));
	p->p_flag |= P_INEXEC;
	PROC_UNLOCK(p);

	/*
	 * Initialize part of the common data
	 */
	bzero(imgp, sizeof(*imgp));
	imgp->proc = p;
	imgp->attr = &attr;
	imgp->args = args;
	oldcred = p->p_ucred;
	orig_osrel = p->p_osrel;
	orig_fctl0 = p->p_fctl0;

#ifdef MAC
	if (umac != NULL) {
		error = copyin_mac(umac, &extmac);
		if (error)
			goto exec_fail;
		mac_p = &extmac;
	} else
		mac_p = NULL;
	error = mac_execve_enter(imgp, mac_p);
	if (error)
		goto exec_fail;
#endif

	/*
	 * Translate the file name. namei() returns a vnode pointer
	 *	in ni_vp among other things.
	 *
	 * XXXAUDIT: It would be desirable to also audit the name of the
	 * interpreter if this is an interpreted binary.
	 */
	if (args->fname != NULL) {
		NDINIT(&nd, LOOKUP, ISOPEN | LOCKLEAF | LOCKSHARED | FOLLOW |
		    SAVENAME | AUDITVNODE1, UIO_SYSSPACE, PTR2CAP(args->fname),
		    td);
	}

	SDT_PROBE1(proc, , , exec, args->fname);

interpret:
	if (args->fname != NULL) {
#ifdef CAPABILITY_MODE
		/*
		 * While capability mode can't reach this point via direct
		 * path arguments to execve(), we also don't allow
		 * interpreters to be used in capability mode (for now).
		 * Catch indirect lookups and return a permissions error.
		 */
		if (IN_CAPABILITY_MODE(td)) {
			error = ECAPMODE;
			goto exec_fail;
		}
#endif
		error = namei(&nd);
		if (error)
			goto exec_fail;

		newtextvp = nd.ni_vp;
		imgp->vp = newtextvp;
	} else {
		AUDIT_ARG_FD(args->fd);
		/*
		 * Descriptors opened only with O_EXEC or O_RDONLY are allowed.
		 */
		error = fgetvp_exec(td, args->fd, &cap_fexecve_rights, &newtextvp);
		if (error)
			goto exec_fail;
		vn_lock(newtextvp, LK_SHARED | LK_RETRY);
		AUDIT_ARG_VNODE1(newtextvp);
		imgp->vp = newtextvp;
	}

	/*
	 * Check file permissions.  Also 'opens' file and sets its vnode to
	 * text mode.
	 */
	error = exec_check_permissions(imgp);
	if (error)
		goto exec_fail_dealloc;

	imgp->object = imgp->vp->v_object;
	if (imgp->object != NULL)
		vm_object_reference(imgp->object);

	error = exec_map_first_page(imgp);
	if (error)
		goto exec_fail_dealloc;

	imgp->proc->p_osrel = 0;
	imgp->proc->p_fctl0 = 0;

	/*
	 * Implement image setuid/setgid.
	 *
	 * Determine new credentials before attempting image activators
	 * so that it can be used by process_exec handlers to determine
	 * credential/setid changes.
	 *
	 * Don't honor setuid/setgid if the filesystem prohibits it or if
	 * the process is being traced.
	 *
	 * We disable setuid/setgid/etc in capability mode on the basis
	 * that most setugid applications are not written with that
	 * environment in mind, and will therefore almost certainly operate
	 * incorrectly. In principle there's no reason that setugid
	 * applications might not be useful in capability mode, so we may want
	 * to reconsider this conservative design choice in the future.
	 *
	 * XXXMAC: For the time being, use NOSUID to also prohibit
	 * transitions on the file system.
	 */
	credential_changing = 0;
	credential_changing |= (attr.va_mode & S_ISUID) &&
	    oldcred->cr_uid != attr.va_uid;
	credential_changing |= (attr.va_mode & S_ISGID) &&
	    oldcred->cr_gid != attr.va_gid;
#ifdef MAC
	will_transition = mac_vnode_execve_will_transition(oldcred, imgp->vp,
	    interpvplabel, imgp);
	credential_changing |= will_transition;
#endif

	/* Don't inherit PROC_PDEATHSIG_CTL value if setuid/setgid. */
	if (credential_changing)
		imgp->proc->p_pdeathsig = 0;

	if (credential_changing &&
#ifdef CAPABILITY_MODE
	    ((oldcred->cr_flags & CRED_FLAG_CAPMODE) == 0) &&
#endif
	    (imgp->vp->v_mount->mnt_flag & MNT_NOSUID) == 0 &&
	    (p->p_flag & P_TRACED) == 0) {
		imgp->credential_setid = true;
		VOP_UNLOCK(imgp->vp);
		imgp->newcred = crdup(oldcred);
		if (attr.va_mode & S_ISUID) {
			euip = uifind(attr.va_uid);
			change_euid(imgp->newcred, euip);
		}
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
		if (attr.va_mode & S_ISGID)
			change_egid(imgp->newcred, attr.va_gid);
		/*
		 * Implement correct POSIX saved-id behavior.
		 *
		 * XXXMAC: Note that the current logic will save the
		 * uid and gid if a MAC domain transition occurs, even
		 * though maybe it shouldn't.
		 */
		change_svuid(imgp->newcred, imgp->newcred->cr_uid);
		change_svgid(imgp->newcred, imgp->newcred->cr_gid);
	} else {
		/*
		 * Implement correct POSIX saved-id behavior.
		 *
		 * XXX: It's not clear that the existing behavior is
		 * POSIX-compliant.  A number of sources indicate that the
		 * saved uid/gid should only be updated if the new ruid is
		 * not equal to the old ruid, or the new euid is not equal
		 * to the old euid and the new euid is not equal to the old
		 * ruid.  The FreeBSD code always updates the saved uid/gid.
		 * Also, this code uses the new (replaced) euid and egid as
		 * the source, which may or may not be the right ones to use.
		 */
		if (oldcred->cr_svuid != oldcred->cr_uid ||
		    oldcred->cr_svgid != oldcred->cr_gid) {
			VOP_UNLOCK(imgp->vp);
			imgp->newcred = crdup(oldcred);
			vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
			change_svuid(imgp->newcred, imgp->newcred->cr_uid);
			change_svgid(imgp->newcred, imgp->newcred->cr_gid);
		}
	}
	/* The new credentials are installed into the process later. */

	/*
	 * Do the best to calculate the full path to the image file.
	 */
	if (args->fname != NULL && args->fname[0] == '/')
		imgp->execpath = args->fname;
	else {
		VOP_UNLOCK(imgp->vp);
		if (vn_fullpath(imgp->vp, &imgp->execpath, &imgp->freepath) != 0)
			imgp->execpath = args->fname;
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
	}

	/*
	 *	If the current process has a special image activator it
	 *	wants to try first, call it.   For example, emulating shell
	 *	scripts differently.
	 */
	error = -1;
	if ((img_first = imgp->proc->p_sysent->sv_imgact_try) != NULL)
		error = img_first(imgp);

	/*
	 *	Loop through the list of image activators, calling each one.
	 *	An activator returns -1 if there is no match, 0 on success,
	 *	and an error otherwise.
	 */
	for (i = 0; error == -1 && execsw[i]; ++i) {
		if (execsw[i]->ex_imgact == NULL ||
		    execsw[i]->ex_imgact == img_first) {
			continue;
		}
		error = (*execsw[i]->ex_imgact)(imgp);
	}

	if (error) {
		if (error == -1)
			error = ENOEXEC;
		goto exec_fail_dealloc;
	}

	/*
	 * Special interpreter operation, cleanup and loop up to try to
	 * activate the interpreter.
	 */
	if (imgp->interpreted) {
		exec_unmap_first_page(imgp);
		/*
		 * The text reference needs to be removed for scripts.
		 * There is a short period before we determine that
		 * something is a script where text reference is active.
		 * The vnode lock is held over this entire period
		 * so nothing should illegitimately be blocked.
		 */
		MPASS(imgp->textset);
		VOP_UNSET_TEXT_CHECKED(newtextvp);
		imgp->textset = false;
		/* free name buffer and old vnode */
		if (args->fname != NULL)
			NDFREE(&nd, NDF_ONLY_PNBUF);
#ifdef MAC
		mac_execve_interpreter_enter(newtextvp, &interpvplabel);
#endif
		if (imgp->opened) {
			VOP_CLOSE(newtextvp, FREAD, td->td_ucred, td);
			imgp->opened = 0;
		}
		vput(newtextvp);
		vm_object_deallocate(imgp->object);
		imgp->object = NULL;
		imgp->credential_setid = false;
		if (imgp->newcred != NULL) {
			crfree(imgp->newcred);
			imgp->newcred = NULL;
		}
		imgp->execpath = NULL;
		free(imgp->freepath, M_TEMP);
		imgp->freepath = NULL;
		/* set new name to that of the interpreter */
		NDINIT(&nd, LOOKUP, ISOPEN | LOCKLEAF | LOCKSHARED | FOLLOW |
		    SAVENAME, UIO_SYSSPACE, PTR2CAP(imgp->interpreter_name),
		    td);
		args->fname = imgp->interpreter_name;
		goto interpret;
	}

	/*
	 * NB: We unlock the vnode here because it is believed that none
	 * of the sv_copyout_strings/sv_fixup operations require the vnode.
	 */
	VOP_UNLOCK(imgp->vp);

	if (disallow_high_osrel &&
	    P_OSREL_MAJOR(p->p_osrel) > P_OSREL_MAJOR(__FreeBSD_version)) {
		error = ENOEXEC;
		uprintf("Osrel %d for image %s too high\n", p->p_osrel,
		    imgp->execpath != NULL ? imgp->execpath : "<unresolved>");
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
		goto exec_fail_dealloc;
	}

	/* ABI enforces the use of Capsicum. Switch into capabilities mode. */
	if (SV_PROC_FLAG(p, SV_CAPSICUM))
		sys_cap_enter(td, NULL);

	/*
	 * Copy out strings (args and env) and initialize stack base.
	 */
	error = (*p->p_sysent->sv_copyout_strings)(imgp, &stack_base);
	if (error != 0) {
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
		goto exec_fail_dealloc;
	}

	/*
	 * Stack setup.
	 */
	error = (*p->p_sysent->sv_fixup)(&stack_base, imgp);
	if (error != 0) {
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
		goto exec_fail_dealloc;
	}

	if (args->fdp != NULL) {
		/* Install a brand new file descriptor table. */
		fdinstall_remapped(td, args->fdp);
		args->fdp = NULL;
	} else {
		/*
		 * Keep on using the existing file descriptor table. For
		 * security and other reasons, the file descriptor table
		 * cannot be shared after an exec.
		 */
		fdunshare(td);
		pdunshare(td);
		/* close files on exec */
		fdcloseexec(td);
	}

	/*
	 * Malloc things before we need locks.
	 */
	i = exec_args_get_begin_envv(imgp->args) - imgp->args->begin_argv;
	/* Cache arguments if they fit inside our allowance */
	if (ps_arg_cache_limit >= i + sizeof(struct pargs)) {
		newargs = pargs_alloc(i);
		bcopy(imgp->args->begin_argv, newargs->ar_args, i);
	}

	/*
	 * For security and other reasons, signal handlers cannot
	 * be shared after an exec. The new process gets a copy of the old
	 * handlers. In execsigs(), the new process will have its signals
	 * reset.
	 */
	if (sigacts_shared(p->p_sigacts)) {
		oldsigacts = p->p_sigacts;
		newsigacts = sigacts_alloc();
		sigacts_copy(newsigacts, oldsigacts);
	}

	vn_lock(imgp->vp, LK_SHARED | LK_RETRY);

	PROC_LOCK(p);
	if (oldsigacts)
		p->p_sigacts = newsigacts;
	/* Stop profiling */
	stopprofclock(p);

	/* reset caught signals */
	execsigs(p);

	/* name this process - nameiexec(p, ndp) */
	bzero(p->p_comm, sizeof(p->p_comm));
	if (args->fname)
		bcopy(nd.ni_cnd.cn_nameptr, p->p_comm,
		    min(nd.ni_cnd.cn_namelen, MAXCOMLEN));
	else if (vn_commname(newtextvp, p->p_comm, sizeof(p->p_comm)) != 0)
		bcopy(fexecv_proc_title, p->p_comm, sizeof(fexecv_proc_title));
	bcopy(p->p_comm, td->td_name, sizeof(td->td_name));
#ifdef KTR
	sched_clear_tdname(td);
#endif

	/*
	 * mark as execed, wakeup the process that vforked (if any) and tell
	 * it that it now has its own resources back
	 */
	p->p_flag |= P_EXEC;
	if ((p->p_flag2 & P2_NOTRACE_EXEC) == 0)
		p->p_flag2 &= ~P2_NOTRACE;
	if ((p->p_flag2 & P2_STKGAP_DISABLE_EXEC) == 0)
		p->p_flag2 &= ~P2_STKGAP_DISABLE;
	if (p->p_flag & P_PPWAIT) {
		p->p_flag &= ~(P_PPWAIT | P_PPTRACE);
		cv_broadcast(&p->p_pwait);
		/* STOPs are no longer ignored, arrange for AST */
		signotify(td);
	}

	/*
	 * Implement image setuid/setgid installation.
	 */
	if (imgp->credential_setid) {
		/*
		 * Turn off syscall tracing for set-id programs, except for
		 * root.  Record any set-id flags first to make sure that
		 * we do not regain any tracing during a possible block.
		 */
		setsugid(p);

#ifdef KTRACE
		if (p->p_tracecred != NULL &&
		    priv_check_cred(p->p_tracecred, PRIV_DEBUG_DIFFCRED))
			ktrprocexec(p, &tracecred, &tracevp);
#endif
		/*
		 * Close any file descriptors 0..2 that reference procfs,
		 * then make sure file descriptors 0..2 are in use.
		 *
		 * Both fdsetugidsafety() and fdcheckstd() may call functions
		 * taking sleepable locks, so temporarily drop our locks.
		 */
		PROC_UNLOCK(p);
		VOP_UNLOCK(imgp->vp);
		fdsetugidsafety(td);
		error = fdcheckstd(td);
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
		if (error != 0)
			goto exec_fail_dealloc;
		PROC_LOCK(p);
#ifdef MAC
		if (will_transition) {
			mac_vnode_execve_transition(oldcred, imgp->newcred,
			    imgp->vp, interpvplabel, imgp);
		}
#endif
	} else {
		if (oldcred->cr_uid == oldcred->cr_ruid &&
		    oldcred->cr_gid == oldcred->cr_rgid)
			p->p_flag &= ~P_SUGID;
	}
	/*
	 * Set the new credentials.
	 */
	if (imgp->newcred != NULL) {
		proc_set_cred(p, imgp->newcred);
		crfree(oldcred);
		oldcred = NULL;
	}

	/*
	 * Store the vp for use in procfs.  This vnode was referenced by namei
	 * or fgetvp_exec.
	 */
	oldtextvp = p->p_textvp;
	p->p_textvp = newtextvp;

#ifdef KDTRACE_HOOKS
	/*
	 * Tell the DTrace fasttrap provider about the exec if it
	 * has declared an interest.
	 */
	if (dtrace_fasttrap_exec)
		dtrace_fasttrap_exec(p);
#endif

	/*
	 * Notify others that we exec'd, and clear the P_INEXEC flag
	 * as we're now a bona fide freshly-execed process.
	 */
	KNOTE_LOCKED(p->p_klist, NOTE_EXEC);
	p->p_flag &= ~P_INEXEC;

	/* clear "fork but no exec" flag, as we _are_ execing */
	p->p_acflag &= ~AFORK;

	/*
	 * Free any previous argument cache and replace it with
	 * the new argument cache, if any.
	 */
	oldargs = p->p_args;
	p->p_args = newargs;
	newargs = NULL;

	PROC_UNLOCK(p);

#ifdef	HWPMC_HOOKS
	/*
	 * Check if system-wide sampling is in effect or if the
	 * current process is using PMCs.  If so, do exec() time
	 * processing.  This processing needs to happen AFTER the
	 * P_INEXEC flag is cleared.
	 */
	if (PMC_SYSTEM_SAMPLING_ACTIVE() || PMC_PROC_IS_USING_PMCS(p)) {
		VOP_UNLOCK(imgp->vp);
		pe.pm_credentialschanged = credential_changing;
		pe.pm_entryaddr = imgp->entry_addr;

		PMC_CALL_HOOK_X(td, PMC_FN_PROCESS_EXEC, (void *) &pe);
		vn_lock(imgp->vp, LK_SHARED | LK_RETRY);
	}
#endif

	/* Set values passed into the program in registers. */
	(*p->p_sysent->sv_setregs)(td, imgp, stack_base);

	VOP_MMAPPED(imgp->vp);

	SDT_PROBE1(proc, , , exec__success, args->fname);

exec_fail_dealloc:
	if (error != 0) {
		p->p_osrel = orig_osrel;
		p->p_fctl0 = orig_fctl0;
	}

	if (imgp->firstpage != NULL)
		exec_unmap_first_page(imgp);

	if (imgp->vp != NULL) {
		if (args->fname)
			NDFREE(&nd, NDF_ONLY_PNBUF);
		if (imgp->opened)
			VOP_CLOSE(imgp->vp, FREAD, td->td_ucred, td);
		if (imgp->textset)
			VOP_UNSET_TEXT_CHECKED(imgp->vp);
		if (error != 0)
			vput(imgp->vp);
		else
			VOP_UNLOCK(imgp->vp);
	}

	if (imgp->object != NULL)
		vm_object_deallocate(imgp->object);

	free(imgp->freepath, M_TEMP);

	if (error == 0) {
		if (p->p_ptevents & PTRACE_EXEC) {
			PROC_LOCK(p);
			if (p->p_ptevents & PTRACE_EXEC)
				td->td_dbgflags |= TDB_EXEC;
			PROC_UNLOCK(p);
		}
	} else {
exec_fail:
		/* we're done here, clear P_INEXEC */
		PROC_LOCK(p);
		p->p_flag &= ~P_INEXEC;
		PROC_UNLOCK(p);

		SDT_PROBE1(proc, , , exec__failure, error);
	}

	if (imgp->newcred != NULL && oldcred != NULL)
		crfree(imgp->newcred);

#ifdef MAC
	mac_execve_exit(imgp);
	mac_execve_interpreter_exit(interpvplabel);
#endif
	exec_free_args(args);

	/*
	 * Handle deferred decrement of ref counts.
	 */
	if (oldtextvp != NULL)
		vrele(oldtextvp);
#ifdef KTRACE
	if (tracevp != NULL)
		vrele(tracevp);
	if (tracecred != NULL)
		crfree(tracecred);
#endif
	pargs_drop(oldargs);
	pargs_drop(newargs);
	if (oldsigacts != NULL)
		sigacts_free(oldsigacts);
	if (euip != NULL)
		uifree(euip);

	if (error && imgp->vmspace_destroyed) {
		/* sorry, no more process anymore. exit gracefully */
		exec_cleanup(td, oldvmspace);
		exit1(td, 0, SIGABRT);
		/* NOT REACHED */
	}

#ifdef KTRACE
	if (error == 0)
		ktrprocctor(p);
#endif

	/*
	 * We don't want cpu_set_syscall_retval() to overwrite any of
	 * the register values put in place by exec_setregs().
	 * Implementations of cpu_set_syscall_retval() will leave
	 * registers unmodified when returning EJUSTRETURN.
	 */
	return (error == 0 ? EJUSTRETURN : error);
}

void
exec_cleanup(struct thread *td, struct vmspace *oldvmspace)
{
	if ((td->td_pflags & TDP_EXECVMSPC) != 0) {
		KASSERT(td->td_proc->p_vmspace != oldvmspace,
		    ("oldvmspace still used"));
		vmspace_free(oldvmspace);
		td->td_pflags &= ~TDP_EXECVMSPC;
	}
}

int
exec_map_first_page(struct image_params *imgp)
{
	vm_object_t object;
	vm_page_t m;
	int error;

	if (imgp->firstpage != NULL)
		exec_unmap_first_page(imgp);

	object = imgp->vp->v_object;
	if (object == NULL)
		return (EACCES);
#if VM_NRESERVLEVEL > 0
	if ((object->flags & OBJ_COLORED) == 0) {
		VM_OBJECT_WLOCK(object);
		vm_object_color(object, 0);
		VM_OBJECT_WUNLOCK(object);
	}
#endif
	error = vm_page_grab_valid_unlocked(&m, object, 0,
	    VM_ALLOC_COUNT(VM_INITIAL_PAGEIN) |
            VM_ALLOC_NORMAL | VM_ALLOC_NOBUSY | VM_ALLOC_WIRED);

	if (error != VM_PAGER_OK)
		return (EIO);
	imgp->firstpage = sf_buf_alloc(m, 0);
	imgp->image_header = (char *)sf_buf_kva(imgp->firstpage);

	return (0);
}

void
exec_unmap_first_page(struct image_params *imgp)
{
	vm_page_t m;

	if (imgp->firstpage != NULL) {
		m = sf_buf_page(imgp->firstpage);
		sf_buf_free(imgp->firstpage);
		imgp->firstpage = NULL;
		vm_page_unwire(m, PQ_ACTIVE);
	}
}

/*
 * Destroy old address space, and allocate a new stack.
 *	The new stack is only sgrowsiz large because it is grown
 *	automatically on a page fault.
 */
int
exec_new_vmspace(struct image_params *imgp, struct sysentvec *sv)
{
	int error;
	struct proc *p = imgp->proc;
	struct vmspace *vmspace = p->p_vmspace;
	struct thread *td = curthread;
	vm_object_t obj;
	struct rlimit rlim_stack;
	vm_offset_t sv_minuser;
	vm_pointer_t stack_addr;
#if __has_feature(capabilities)
	vm_pointer_t strings_addr;
	register_t perms;
#endif
	vm_map_t map;
	vm_prot_t stack_prot;
	u_long ssiz;
	vm_pointer_t shared_page_addr;

	imgp->vmspace_destroyed = 1;
	imgp->sysent = sv;

	sigfastblock_clear(td);
	umtx_exec(p);
	itimers_exec(p);
	if (sv->sv_onexec != NULL)
		sv->sv_onexec(p, imgp);

	EVENTHANDLER_DIRECT_INVOKE(process_exec, p, imgp);

	/*
	 * Blow away entire process VM, if address space not shared,
	 * otherwise, create a new VM space so that other threads are
	 * not disrupted
	 */
	map = &vmspace->vm_map;
	if (map_at_zero)
		sv_minuser = sv->sv_minuser;
	else
		sv_minuser = MAX(sv->sv_minuser, PAGE_SIZE);
	if (refcount_load(&vmspace->vm_refcnt) == 1 &&
	    vm_map_min(map) == sv_minuser &&
	    vm_map_max(map) == sv->sv_maxuser &&
	    cpu_exec_vmspace_reuse(p, map)) {
		shmexit(vmspace);
		pmap_remove_pages(vmspace_pmap(vmspace));
		vm_map_clear(map);
		/*
		 * An exec terminates mlockall(MCL_FUTURE).
		 * ASLR and W^X states must be re-evaluated.
		 */
		vm_map_lock(map);
		vm_map_modflags(map, 0, MAP_WIREFUTURE | MAP_ASLR |
		    MAP_ASLR_IGNSTART | MAP_WXORX);
		vm_map_unlock(map);
	} else {
		error = vmspace_exec(p, sv_minuser, sv->sv_maxuser);
		if (error)
			return (error);
		vmspace = p->p_vmspace;
		map = &vmspace->vm_map;
	}
	map->flags |= imgp->map_flags;
	if (sv->sv_flags & SV_CHERI)
		map->flags |= MAP_RESERVATIONS;
	else
		map->flags &= ~MAP_RESERVATIONS;

#ifdef CPU_QEMU_MALTA
	if (curthread->td_md.md_flags & MDTD_QTRACE) {
		char buffer[128];

		snprintf(buffer, sizeof(buffer), "VMMAP %d: exec",
		    curproc->p_pid);
		CHERI_TRACE_STRING(buffer);
	}
#endif

	/* Map a shared page */
	obj = sv->sv_shared_page_obj;
	if (obj != NULL) {
		vm_object_reference(obj);
		shared_page_addr = sv->sv_shared_page_base;
#if __has_feature(capabilities)
		error = vm_map_reservation_create(map, &shared_page_addr,
		    sv->sv_shared_page_len, PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_EXECUTE);
		if (error != KERN_SUCCESS) {
			vm_object_deallocate(obj);
			return (vm_mmap_to_errno(error));
		}
#endif
		error = vm_map_fixed(map, obj, 0,
		    shared_page_addr, sv->sv_shared_page_len,
		    VM_PROT_READ | VM_PROT_EXECUTE,
		    VM_PROT_READ | VM_PROT_EXECUTE,
		    MAP_INHERIT_SHARE | MAP_ACC_NO_CHARGE);
		if (error != KERN_SUCCESS) {
#if __has_feature(capabilities)
			vm_map_reservation_delete(map, shared_page_addr);
#endif
			vm_object_deallocate(obj);
			return (vm_mmap_to_errno(error));
		}
	}

	/* Allocate a new stack */
	if (imgp->stack_sz != 0) {
		ssiz = trunc_page(imgp->stack_sz);
		PROC_LOCK(p);
		lim_rlimit_proc(p, RLIMIT_STACK, &rlim_stack);
		PROC_UNLOCK(p);
		if (ssiz > rlim_stack.rlim_max)
			ssiz = rlim_stack.rlim_max;
		if (ssiz > rlim_stack.rlim_cur) {
			rlim_stack.rlim_cur = ssiz;
			kern_setrlimit(curthread, RLIMIT_STACK, &rlim_stack);
		}
	} else if (sv->sv_maxssiz != NULL) {
		ssiz = *sv->sv_maxssiz;
	} else {
		ssiz = maxssiz;
	}
#if __has_feature(capabilities)
	/*
	 * NB: This may cause the stack to exceed the administrator-
	 * configured stack size limit.
	 */
	ssiz = CHERI_REPRESENTABLE_LENGTH(ssiz);
#endif
	imgp->eff_stack_sz = lim_cur(curthread, RLIMIT_STACK);
	if (ssiz < imgp->eff_stack_sz)
		imgp->eff_stack_sz = ssiz;
	p->p_usrstack = sv->sv_usrstack;
#if __has_feature(capabilities)
	p->p_usrstack = CHERI_REPRESENTABLE_BASE(p->p_usrstack, ssiz);
#endif

	/* We reserve the whole max stack size with restricted permission */
	stack_addr = p->p_usrstack - ssiz;
	stack_prot = obj != NULL && imgp->stack_prot != 0 ?
	    imgp->stack_prot : sv->sv_stackprot;
	imgp->stack_sz = ssiz;
	error = vm_map_reservation_create(map, &stack_addr, ssiz,
	    PAGE_SIZE, stack_prot);
	if (error != KERN_SUCCESS)
		return (vm_mmap_to_errno(error));

	error = vm_map_stack(map, stack_addr, (vm_size_t)ssiz, stack_prot,
	    stack_prot, MAP_STACK_GROWS_DOWN);
	if (error != KERN_SUCCESS) {
		uprintf("exec_new_vmspace: mapping stack size %#jx prot %#x "
		    "failed mach error %d errno %d\n", (uintmax_t)ssiz,
		    stack_prot, error, vm_mmap_to_errno(error));
		vm_map_reservation_delete(map, stack_addr);
		return (vm_mmap_to_errno(error));
	}

#if __has_feature(capabilities)
	perms = (~CHERI_PROT2PERM_MASK | vm_map_prot2perms(stack_prot)) &
	    CHERI_CAP_USER_DATA_PERMS;
#ifdef __CHERI_PURE_CAPABILITY__
	imgp->stack = (void *)cheri_andperm(stack_addr + ssiz, perms);
#else
	imgp->stack = cheri_capability_build_user_data(perms, stack_addr,
	    ssiz, ssiz);
#endif

	if (sv->sv_flags & SV_CHERI) {
		/*
		 * Map a seperate space for strings outside the stack.
		 * We currently place it just just below the stack as
		 * this avoides collisions with init which is linked
		 * near the bottom of the address space.
		 */
		strings_addr =
		    CHERI_REPRESENTABLE_BASE(stack_addr - ARG_MAX, ARG_MAX);
		error = vm_mmap_object(map, &strings_addr, 0, ARG_MAX,
		    VM_PROT_RW_CAP, VM_PROT_RW_CAP, MAP_ANON | MAP_FIXED |
		    MAP_RESERVATION_CREATE, NULL, 0, FALSE, td);
		if (error != KERN_SUCCESS)
			return (vm_mmap_to_errno(error));
#ifdef __CHERI_PURE_CAPABILITY__
		imgp->strings = (void *)strings_addr;
#else
		imgp->strings = cheri_capability_build_user_data(
		    CHERI_CAP_USER_DATA_PERMS, strings_addr, ARG_MAX, ARG_MAX);
#endif
	} else
		imgp->strings = imgp->stack;

	if (sv->sv_flags & SV_CHERI)
		p->p_psstrings = strings_addr + ARG_MAX - sv->sv_szpsstrings;
	else
#endif
		p->p_psstrings = p->p_usrstack - sv->sv_szpsstrings;

	/*
	 * vm_ssize and vm_maxsaddr are somewhat antiquated concepts, but they
	 * are still used to enforce the stack rlimit on the process stack.
	 */
	vmspace->vm_ssize = sgrowsiz >> PAGE_SHIFT;
	vmspace->vm_maxsaddr = stack_addr;

	return (0);
}

/*
 * Takes a pointer to a pointer an array of pointers in userspace, loads
 * the loads the current value and updates the array pointer.
 */
static int
get_argenv_ptr(void * __capability *arrayp, void * __capability *ptrp)
{
	uintcap_t ptr;
	char * __capability array;
#ifdef COMPAT_FREEBSD32
	uint32_t ptr32;
#endif
#ifdef COMPAT_FREEBSD64
	uint64_t ptr64;
#endif

	array = *arrayp;
#ifdef COMPAT_FREEBSD32
	if (SV_CURPROC_FLAG(SV_ILP32)) {
		if (fueword32(array, &ptr32) == -1)
			return (EFAULT);
		array += sizeof(ptr32);
		*ptrp = __USER_CAP_STR((void *)(uintptr_t)ptr32);
	} else
#endif
#ifdef COMPAT_FREEBSD64
	if (SV_CURPROC_FLAG(SV_LP64 | SV_CHERI) == SV_LP64) {
		if (fueword64(array, &ptr64) == -1)
			return (EFAULT);
		array += sizeof(ptr64);
		*ptrp = __USER_CAP_STR((void *)(uintptr_t)ptr64);
	} else
#endif
	{
		if (fuecap(array, &ptr) == -1)
			return (EFAULT);
		array += sizeof(ptr);
		*ptrp = (void * __capability)ptr;
	}
	*arrayp = array;
	return (0);
}

/*
 * Copy out argument and environment strings from the old process address
 * space into the temporary string buffer.
 */
int
exec_copyin_args(struct image_args *args, const char * __capability fname,
    enum uio_seg segflg, void * __capability argv, void * __capability envv)
{
	void * __capability ptr;
	int error;

	bzero(args, sizeof(*args));
	if (argv == NULL)
		return (EFAULT);

	/*
	 * Allocate demand-paged memory for the file name, argument, and
	 * environment strings.
	 */
	error = exec_alloc_args(args);
	if (error != 0)
		return (error);

	/*
	 * Copy the file name.
	 */
	error = exec_args_add_fname(args, fname, segflg);
	if (error != 0)
		goto err_exit;

	/*
	 * extract arguments first
	 */
	for (;;) {
		error = get_argenv_ptr(&argv, &ptr);
		if (error != 0)
			goto err_exit;
		if (ptr == NULL)
			break;
		error = exec_args_add_arg(args, ptr, UIO_USERSPACE);
		if (error != 0)
			goto err_exit;
	}

	/*
	 * extract environment strings
	 */
	if (envv) {
		for (;;) {
			error = get_argenv_ptr(&envv, &ptr);
			if (error != 0)
				goto err_exit;
			if (ptr == NULL)
				break;
			error = exec_args_add_env(args, ptr, UIO_USERSPACE);
			if (error != 0)
				goto err_exit;
		}
	}

	return (0);

err_exit:
	exec_free_args(args);
	return (error);
}

int
exec_copyin_data_fds(struct thread *td, struct image_args *args,
    const void * __capability data, size_t datalen,
    const int * __capability fds, size_t fdslen)
{
	struct filedesc *ofdp;
	const char *p;
	int *kfds;
	int error;

	memset(args, '\0', sizeof(*args));
	ofdp = td->td_proc->p_fd;
	if (datalen >= ARG_MAX || fdslen >= ofdp->fd_nfiles)
		return (E2BIG);
	error = exec_alloc_args(args);
	if (error != 0)
		return (error);

	args->begin_argv = args->buf;
	args->stringspace = ARG_MAX;

	if (datalen > 0) {
		/*
		 * Argument buffer has been provided. Copy it into the
		 * kernel as a single string and add a terminating null
		 * byte.
		 */
		error = copyin(data, args->begin_argv, datalen);
		if (error != 0)
			goto err_exit;
		args->begin_argv[datalen] = '\0';
		args->endp = args->begin_argv + datalen + 1;
		args->stringspace -= datalen + 1;

		/*
		 * Traditional argument counting. Count the number of
		 * null bytes.
		 */
		for (p = args->begin_argv; p < args->endp; ++p)
			if (*p == '\0')
				++args->argc;
	} else {
		/* No argument buffer provided. */
		args->endp = args->begin_argv;
	}

	/* Create new file descriptor table. */
	kfds = malloc(fdslen * sizeof(int), M_TEMP, M_WAITOK);
	error = copyin(fds, kfds, fdslen * sizeof(int));
	if (error != 0) {
		free(kfds, M_TEMP);
		goto err_exit;
	}
	error = fdcopy_remapped(ofdp, kfds, fdslen, &args->fdp);
	free(kfds, M_TEMP);
	if (error != 0)
		goto err_exit;

	return (0);
err_exit:
	exec_free_args(args);
	return (error);
}

struct exec_args_kva {
	vm_pointer_t addr;
	u_int gen;
	SLIST_ENTRY(exec_args_kva) next;
};

DPCPU_DEFINE_STATIC(struct exec_args_kva *, exec_args_kva);

static SLIST_HEAD(, exec_args_kva) exec_args_kva_freelist;
static struct mtx exec_args_kva_mtx;
static u_int exec_args_gen;

static void
exec_prealloc_args_kva(void *arg __unused)
{
	struct exec_args_kva *argkva;
	u_int i;

	SLIST_INIT(&exec_args_kva_freelist);
	mtx_init(&exec_args_kva_mtx, "exec args kva", NULL, MTX_DEF);
	for (i = 0; i < exec_map_entries; i++) {
		argkva = malloc(sizeof(*argkva), M_PARGS, M_WAITOK);
		argkva->addr = kmap_alloc_wait(exec_map, exec_map_entry_size);
		argkva->gen = exec_args_gen;
		SLIST_INSERT_HEAD(&exec_args_kva_freelist, argkva, next);
	}
}
SYSINIT(exec_args_kva, SI_SUB_EXEC, SI_ORDER_ANY, exec_prealloc_args_kva, NULL);

static vm_pointer_t
exec_alloc_args_kva(void **cookie)
{
	struct exec_args_kva *argkva;

	argkva = (void *)atomic_readandclear_ptr(
	    (uintptr_t *)DPCPU_PTR(exec_args_kva));
	if (argkva == NULL) {
		mtx_lock(&exec_args_kva_mtx);
		while ((argkva = SLIST_FIRST(&exec_args_kva_freelist)) == NULL)
			(void)mtx_sleep(&exec_args_kva_freelist,
			    &exec_args_kva_mtx, 0, "execkva", 0);
		SLIST_REMOVE_HEAD(&exec_args_kva_freelist, next);
		mtx_unlock(&exec_args_kva_mtx);
	}
	*(struct exec_args_kva **)cookie = argkva;
	return (argkva->addr);
}

static void
exec_release_args_kva(struct exec_args_kva *argkva, u_int gen)
{
	vm_offset_t base;

	base = argkva->addr;
	if (argkva->gen != gen) {
		(void)vm_map_madvise(exec_map, base, base + exec_map_entry_size,
		    MADV_FREE);
		argkva->gen = gen;
	}
	if (!atomic_cmpset_ptr((uintptr_t *)DPCPU_PTR(exec_args_kva),
	    (uintptr_t)NULL, (uintptr_t)argkva)) {
		mtx_lock(&exec_args_kva_mtx);
		SLIST_INSERT_HEAD(&exec_args_kva_freelist, argkva, next);
		wakeup_one(&exec_args_kva_freelist);
		mtx_unlock(&exec_args_kva_mtx);
	}
}

static void
exec_free_args_kva(void *cookie)
{

	exec_release_args_kva(cookie, exec_args_gen);
}

static void
exec_args_kva_lowmem(void *arg __unused)
{
	SLIST_HEAD(, exec_args_kva) head;
	struct exec_args_kva *argkva;
	u_int gen;
	int i;

	gen = atomic_fetchadd_int(&exec_args_gen, 1) + 1;

	/*
	 * Force an madvise of each KVA range. Any currently allocated ranges
	 * will have MADV_FREE applied once they are freed.
	 */
	SLIST_INIT(&head);
	mtx_lock(&exec_args_kva_mtx);
	SLIST_SWAP(&head, &exec_args_kva_freelist, exec_args_kva);
	mtx_unlock(&exec_args_kva_mtx);
	while ((argkva = SLIST_FIRST(&head)) != NULL) {
		SLIST_REMOVE_HEAD(&head, next);
		exec_release_args_kva(argkva, gen);
	}

	CPU_FOREACH(i) {
		argkva = (void *)atomic_readandclear_ptr(
		    (uintptr_t *)DPCPU_ID_PTR(i, exec_args_kva));
		if (argkva != NULL)
			exec_release_args_kva(argkva, gen);
	}
}
EVENTHANDLER_DEFINE(vm_lowmem, exec_args_kva_lowmem, NULL,
    EVENTHANDLER_PRI_ANY);

/*
 * Allocate temporary demand-paged, zero-filled memory for the file name,
 * argument, and environment strings.
 */
int
exec_alloc_args(struct image_args *args)
{

	args->buf = (char *)exec_alloc_args_kva(&args->bufkva);
	return (0);
}

void
exec_free_args(struct image_args *args)
{

	if (args->buf != NULL) {
		exec_free_args_kva(args->bufkva);
		args->buf = NULL;
	}
	if (args->fname_buf != NULL) {
		free(args->fname_buf, M_TEMP);
		args->fname_buf = NULL;
	}
	if (args->fdp != NULL)
		fdescfree_remapped(args->fdp);
}

/*
 * A set to functions to fill struct image args.
 *
 * NOTE: exec_args_add_fname() must be called (possibly with a NULL
 * fname) before the other functions.  All exec_args_add_arg() calls must
 * be made before any exec_args_add_env() calls.  exec_args_adjust_args()
 * may be called any time after exec_args_add_fname().
 *
 * exec_args_add_fname() - install path to be executed
 * exec_args_add_arg() - append an argument string
 * exec_args_add_env() - append an env string
 * exec_args_adjust_args() - adjust location of the argument list to
 *                           allow new arguments to be prepended
 */
int
exec_args_add_fname(struct image_args *args, const char * __capability fname,
    enum uio_seg segflg)
{
	int error;
	size_t length;

	KASSERT(args->fname == NULL, ("fname already appended"));
	KASSERT(args->endp == NULL, ("already appending to args"));

	if (fname != NULL) {
		args->fname = args->buf;
		if (segflg == UIO_SYSSPACE)
			error = copystr((__cheri_fromcap const char *)fname,
			    args->fname, PATH_MAX, &length);
		else
			error = copyinstr(fname, args->fname, PATH_MAX,
			    &length);
		if (error != 0)
			return (error == ENAMETOOLONG ? E2BIG : error);
	} else
		length = 0;

	/* Set up for _arg_*()/_env_*() */
	args->endp = args->buf + length;
	/* begin_argv must be set and kept updated */
	args->begin_argv = args->endp;
	KASSERT(exec_map_entry_size - length >= ARG_MAX,
	    ("too little space remaining for arguments %zu < %zu",
	    exec_map_entry_size - length, (size_t)ARG_MAX));
	args->stringspace = ARG_MAX;

	return (0);
}

static int
exec_args_add_str(struct image_args *args, const char * __capability str,
    enum uio_seg segflg, int *countp)
{
	int error;
	size_t length;

	KASSERT(args->endp != NULL, ("endp not initialized"));
	KASSERT(args->begin_argv != NULL, ("begin_argp not initialized"));

	if (segflg == UIO_SYSSPACE)
		error = copystr((__cheri_fromcap const char *)str, args->endp,
		    args->stringspace, &length);
	else
		error = copyinstr(str, args->endp, args->stringspace,
		    &length);
	if (error != 0)
		return (error == ENAMETOOLONG ? E2BIG : error);
	args->stringspace -= length;
	args->endp += length;
	(*countp)++;

	return (0);
}

int
exec_args_add_arg(struct image_args *args, const char * __capability argp,
    enum uio_seg segflg)
{

	KASSERT(args->envc == 0, ("appending args after env"));

	return (exec_args_add_str(args, argp, segflg, &args->argc));
}

int
exec_args_add_env(struct image_args *args, const char * __capability envp,
    enum uio_seg segflg)
{

	if (args->envc == 0)
		args->begin_envv = args->endp;

	return (exec_args_add_str(args, envp, segflg, &args->envc));
}

int
exec_args_adjust_args(struct image_args *args, size_t consume, ssize_t extend)
{
	ssize_t offset;

	KASSERT(args->endp != NULL, ("endp not initialized"));
	KASSERT(args->begin_argv != NULL, ("begin_argp not initialized"));

	offset = extend - consume;
	if (args->stringspace < offset)
		return (E2BIG);
	memmove(args->begin_argv + extend, args->begin_argv + consume,
	    args->endp - args->begin_argv + consume);
	if (args->envc > 0)
		args->begin_envv += offset;
	args->endp += offset;
	args->stringspace -= offset;
	return (0);
}

char *
exec_args_get_begin_envv(struct image_args *args)
{

	KASSERT(args->endp != NULL, ("endp not initialized"));

	if (args->envc > 0)
		return (args->begin_envv);
	return (args->endp);
}

void
exec_stackgap(struct image_params *imgp, uintcap_t *dp)
{
	if (imgp->sysent->sv_stackgap == NULL ||
	    (imgp->proc->p_fctl0 & (NT_FREEBSD_FCTL_ASLR_DISABLE |
	    NT_FREEBSD_FCTL_ASG_DISABLE)) != 0 ||
	    (imgp->map_flags & MAP_ASLR) == 0)
		return;
	imgp->sysent->sv_stackgap(imgp, dp);
}

/*
 * Copy strings out to the new process address space, constructing new arg
 * and env vector tables. Return a pointer to the base so that it can be used
 * as the initial stack pointer.
 *
 * XXX: We may want a wrapper of cheri_setbounds() that warns about
 * capabilities that are overly broad.
 */
int
exec_copyout_strings(struct image_params *imgp, uintcap_t *stack_base)
{
	int argc, envc;
	char * __capability * __capability vectp;
	char *stringp;
	uintcap_t destp, ustringp;
	struct ps_strings * __capability arginfo;
	struct proc *p;
	size_t execpath_len, len;
	int error, szsigcode, szps;
	char canary[sizeof(long) * 8];
	bool strings_on_stack;

	szps = sizeof(pagesizes[0]) * MAXPAGESIZES;
	/*
	 * Calculate string base and vector table pointers.
	 * Also deal with signal trampoline code for this exec type.
	 */
	if (imgp->execpath != NULL && imgp->auxargs != NULL)
		execpath_len = strlen(imgp->execpath) + 1;
	else
		execpath_len = 0;
	p = imgp->proc;
	szsigcode = 0;

	strings_on_stack = true;
#if __has_feature(capabilities)
	if (imgp->stack != imgp->strings)
		strings_on_stack = false;
	destp = (uintcap_t)imgp->strings;
	destp = cheri_setaddress(destp, p->p_psstrings);
	arginfo = (struct ps_strings * __capability)cheri_setboundsexact(destp,
	    sizeof(*arginfo));
#else
	destp = (uintptr_t)p->p_psstrings;
	arginfo = (struct ps_strings *)destp;
#endif
	imgp->ps_strings = arginfo;
	if (p->p_sysent->sv_sigcode_base == 0) {
		if (p->p_sysent->sv_szsigcode != NULL)
			szsigcode = *(p->p_sysent->sv_szsigcode);
	}

	/*
	 * install sigcode
	 */
	if (szsigcode != 0) {
		destp -= szsigcode;
		destp = rounddown2(destp, sizeof(void * __capability));
		error = copyout(p->p_sysent->sv_sigcode,
		    (void * __capability)destp, szsigcode);
		if (error != 0)
			return (error);
	}

	/*
	 * Copy the image path for the rtld.
	 */
	if (execpath_len != 0) {
		destp -= execpath_len;
		destp = rounddown2(destp, sizeof(void * __capability));
#if __has_feature(capabilities)
		imgp->execpathp = (void * __capability)
		    cheri_setboundsexact(destp, execpath_len);
#else
		imgp->execpathp = (void *)destp;
#endif
		error = copyout(imgp->execpath, imgp->execpathp, execpath_len);
		if (error != 0)
			return (error);
	}

	/*
	 * Prepare the canary for SSP.
	 */
	arc4rand(canary, sizeof(canary), 0);
	destp -= sizeof(canary);
#if __has_feature(capabilities)
	imgp->canary = (void * __capability)cheri_setboundsexact(destp,
	    sizeof(canary));
#else
	imgp->canary = (void *)destp;
#endif
	error = copyout(canary, imgp->canary, sizeof(canary));
	if (error != 0)
		return (error);
	imgp->canarylen = sizeof(canary);

	/*
	 * Prepare the pagesizes array.
	 */
	destp -= szps;
	destp = rounddown2(destp, sizeof(void * __capability));
#if __has_feature(capabilities)
	imgp->pagesizes = (void * __capability)cheri_setboundsexact(destp,
	    szps);
#else
	imgp->pagesizes = (void *)destp;
#endif
	error = copyout(pagesizes, imgp->pagesizes, szps);
	if (error != 0)
		return (error);
	imgp->pagesizeslen = szps;

	/*
	 * Allocate room for the argument and environment strings.
	 */
	destp -= ARG_MAX - imgp->args->stringspace;
	destp = rounddown2(destp, sizeof(void * __capability));
#if __has_feature(capabilities)
	ustringp = cheri_setbounds(destp, ARG_MAX - imgp->args->stringspace);
#else
	ustringp = destp;
#endif

	exec_stackgap(imgp, &destp);

	if (imgp->auxargs) {
		/*
		 * Allocate room on the stack for the ELF auxargs
		 * array.  It has up to AT_COUNT entries.
		 */
		destp -= AT_COUNT * sizeof(Elf_Auxinfo);
		destp = rounddown2(destp, sizeof(void * __capability));
	}

	vectp = (char * __capability * __capability)destp;

	/*
	 * Allocate room for the argv[] and env vectors including the
	 * terminating NULL pointers.
	 */
	vectp -= imgp->args->argc + 1 + imgp->args->envc + 1;

	if (!strings_on_stack) {
		*stack_base = (uintcap_t)imgp->stack;
	} else {
		/*
		 * vectp also becomes our initial stack base
		 */
		*stack_base = (uintcap_t)vectp;
	}

	stringp = imgp->args->begin_argv;
	argc = imgp->args->argc;
	envc = imgp->args->envc;

	/*
	 * Copy out strings - arguments and environment.
	 */
	error = copyout(stringp, (void * __capability)ustringp,
	    ARG_MAX - imgp->args->stringspace);
	if (error != 0)
		return (error);

	/*
	 * Fill in "ps_strings" struct for ps, w, etc.
	 */
#if __has_feature(capabilities)
	imgp->argv = cheri_setbounds(vectp, (argc + 1) * sizeof(*vectp));
#else
	imgp->argv = vectp;
#endif
	if (sucap(&arginfo->ps_argvstr, (intcap_t)imgp->argv) != 0 ||
	    suword32(&arginfo->ps_nargvstr, argc) != 0)
		return (EFAULT);

	/*
	 * Fill in argument portion of vector table.
	 */
	for (; argc > 0; --argc) {
		len = strlen(stringp) + 1;
#if __has_feature(capabilities)
		if (sucap(vectp++, cheri_setbounds(ustringp, len)) != 0)
			return (EFAULT);
#else
		if (suword(vectp++, ustringp) != 0)
			return (EFAULT);
#endif
		stringp += len;
		ustringp += len;
	}

	/* a null vector table pointer separates the argp's from the envp's */
	if (suword(vectp++, 0) != 0)
		return (EFAULT);

#if __has_feature(capabilities)
	imgp->envv = cheri_setbounds(vectp, (envc + 1) * sizeof(*vectp));
#else
	imgp->envv = vectp;
#endif
	if (sucap(&arginfo->ps_envstr, (intcap_t)imgp->envv) != 0 ||
	    suword32(&arginfo->ps_nenvstr, envc) != 0)
		return (EFAULT);

	/*
	 * Fill in environment portion of vector table.
	 */
	for (; envc > 0; --envc) {
		len = strlen(stringp) + 1;
#if __has_feature(capabilities)
		if (sucap(vectp++, cheri_setbounds(ustringp, len)) != 0)
			return (EFAULT);
#else
		if (suword(vectp++, ustringp) != 0)
			return (EFAULT);
#endif
		stringp += len;
		ustringp += len;
	}

	/* end of vector table is a null pointer */
	if (suword(vectp, 0) != 0)
		return (EFAULT);

	if (imgp->auxargs) {
		vectp++;
#if __has_feature(capabilities)
		imgp->auxv = cheri_setbounds(vectp,
		    AT_COUNT * sizeof(Elf_Auxinfo));
#else
		imgp->auxv = vectp;
#endif
		error = imgp->sysent->sv_copyout_auxargs(imgp, (uintcap_t)imgp->auxv);
		if (error != 0)
			return (error);
	}

	return (0);
}

/*
 * Check permissions of file to execute.
 *	Called with imgp->vp locked.
 *	Return 0 for success or error code on failure.
 */
int
exec_check_permissions(struct image_params *imgp)
{
	struct vnode *vp = imgp->vp;
	struct vattr *attr = imgp->attr;
	struct thread *td;
	int error;

	td = curthread;

	/* Get file attributes */
	error = VOP_GETATTR(vp, attr, td->td_ucred);
	if (error)
		return (error);

#ifdef MAC
	error = mac_vnode_check_exec(td->td_ucred, imgp->vp, imgp);
	if (error)
		return (error);
#endif

	/*
	 * 1) Check if file execution is disabled for the filesystem that
	 *    this file resides on.
	 * 2) Ensure that at least one execute bit is on. Otherwise, a
	 *    privileged user will always succeed, and we don't want this
	 *    to happen unless the file really is executable.
	 * 3) Ensure that the file is a regular file.
	 */
	if ((vp->v_mount->mnt_flag & MNT_NOEXEC) ||
	    (attr->va_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0 ||
	    (attr->va_type != VREG))
		return (EACCES);

	/*
	 * Zero length files can't be exec'd
	 */
	if (attr->va_size == 0)
		return (ENOEXEC);

	/*
	 *  Check for execute permission to file based on current credentials.
	 */
	error = VOP_ACCESS(vp, VEXEC, td->td_ucred, td);
	if (error)
		return (error);

	/*
	 * Check number of open-for-writes on the file and deny execution
	 * if there are any.
	 *
	 * Add a text reference now so no one can write to the
	 * executable while we're activating it.
	 *
	 * Remember if this was set before and unset it in case this is not
	 * actually an executable image.
	 */
	error = VOP_SET_TEXT(vp);
	if (error != 0)
		return (error);
	imgp->textset = true;

	/*
	 * Call filesystem specific open routine (which does nothing in the
	 * general case).
	 */
	error = VOP_OPEN(vp, FREAD, td->td_ucred, td, NULL);
	if (error == 0)
		imgp->opened = 1;
	return (error);
}

/*
 * Exec handler registration
 */
int
exec_register(const struct execsw *execsw_arg)
{
	const struct execsw **es, **xs, **newexecsw;
	u_int count = 2;	/* New slot and trailing NULL */

	if (execsw)
		for (es = execsw; *es; es++)
			count++;
	newexecsw = malloc(count * sizeof(*es), M_TEMP, M_WAITOK);
	xs = newexecsw;
	if (execsw)
		for (es = execsw; *es; es++)
			*xs++ = *es;
	*xs++ = execsw_arg;
	*xs = NULL;
	if (execsw)
		free(execsw, M_TEMP);
	execsw = newexecsw;
	return (0);
}

int
exec_unregister(const struct execsw *execsw_arg)
{
	const struct execsw **es, **xs, **newexecsw;
	int count = 1;

	if (execsw == NULL)
		panic("unregister with no handlers left?\n");

	for (es = execsw; *es; es++) {
		if (*es == execsw_arg)
			break;
	}
	if (*es == NULL)
		return (ENOENT);
	for (es = execsw; *es; es++)
		if (*es != execsw_arg)
			count++;
	newexecsw = malloc(count * sizeof(*es), M_TEMP, M_WAITOK);
	xs = newexecsw;
	for (es = execsw; *es; es++)
		if (*es != execsw_arg)
			*xs++ = *es;
	*xs = NULL;
	if (execsw)
		free(execsw, M_TEMP);
	execsw = newexecsw;
	return (0);
}
// CHERI CHANGES START
// {
//   "updated": 20200123,
//   "target_type": "kernel",
//   "changes": [
//     "integer_provenance",
//     "user_capabilities"
//   ],
//   "changes_purecap": [
//     "pointer_as_integer"
//   ]
// }
// CHERI CHANGES END
