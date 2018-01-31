/*-
 * Copyright (c) 2002 Doug Rabson
 * Copyright (c) 2015-2018 SRI International
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
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

#include "opt_compat.h"
#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ktrace.h"
#include "opt_posix.h"

#include <sys/param.h>
#include <sys/signal.h>
#include <sys/syscallsubr.h>
#include <sys/user.h>
#include <sys/vnode.h>

#include <cheri/cheri.h>

#include <compat/cheriabi/cheriabi.h>
#include <compat/cheriabi/cheriabi_signal.h>
#include <compat/cheriabi/cheriabi_util.h>
#include <compat/cheriabi/cheriabi_proto.h>

struct sigaction_c {
	void * __capability	sa_u;
	int			sa_flags;
	sigset_t		sa_mask;
};

#ifdef CHERIABI_NEEDS_UPDATE
CTASSERT(sizeof(struct sigaltstack32) == 12);
CTASSERT(sizeof(struct sigaction32) == 24);
#endif

int
cheriabi_sigaction(struct thread *td, struct cheriabi_sigaction_args *uap)
{
	struct sigaction_c sa_c;
	struct sigaction sa, osa, *sap;
	void * __capability cap;

	int error, tag;

	if (uap->act) {
		error = copyincap_c(uap->act, &sa_c, sizeof(sa_c));
		if (error)
			return (error);
		tag = cheri_gettag(sa_c.sa_u);
		if (!tag) {
			sa.sa_handler = (void (*)(int))(uintptr_t)(__intcap_t)sa_c.sa_u;
			if (sa.sa_handler != SIG_DFL &&
			    sa.sa_handler != SIG_IGN) {
				SYSERRCAUSE("untagged sa_handler and not "
				    "SIG_DFL or SIG_IGN (%p)", sa.sa_handler);
				return (EPROT);
			}
		} else {
			error = cheriabi_cap_to_ptr((caddr_t *)&sa.sa_handler,
			    sa_c.sa_u,
			    8 /* XXX-BD: at least two instructions */,
		            CHERI_PERM_LOAD | CHERI_PERM_EXECUTE, 0);
			if (error) {
				SYSERRCAUSE("in cheriabi_cap_to_ptr");
				return (error);
			}
		}
		CP(sa_c, sa, sa_flags);
		CP(sa_c, sa, sa_mask);
		sap = &sa;
		cap = sa_c.sa_u;
	} else
		sap = NULL;
	error = kern_sigaction_cap(td, uap->sig, sap,
	    uap->oact != NULL ? &osa : NULL, 0, &cap);
	if (error != 0)
		SYSERRCAUSE("error in kern_sigaction_cap");
	if (error == 0 && uap->oact != NULL) {
		sa_c.sa_u = cap;
		CP(osa, sa_c, sa_flags);
		CP(osa, sa_c, sa_mask);
		error = copyoutcap_c(&sa_c, uap->oact, sizeof(sa_c));
		if (error != 0) {
			SYSERRCAUSE("error in copyoutcap");
		}
	}
	return (error);
}

int
cheriabi_sigprocmask(struct thread *td, struct cheriabi_sigprocmask_args *uap)
{
	sigset_t set, oset;
	sigset_t *setp, *osetp;
	int error;

	if (uap->set != NULL) {
		error = copyin_c(uap->set, &set, sizeof(set));
		setp = &set;
	} else
		setp = NULL;
	if (uap->oset != NULL)
		osetp = &oset;
	else
		osetp = NULL;
	error = kern_sigprocmask(td, uap->how, setp, osetp, 0);
	if (osetp && error == 0)
		error = copyout_c(
		    (__cheri_tocap sigset_t * __capability)osetp, uap->oset,
		    sizeof(oset));
	return (error);
}

int
cheriabi_sigwait(struct thread *td, struct cheriabi_sigwait_args *uap)
{
	ksiginfo_t ksi;
	sigset_t set;
	int error;

	error = copyin_c(uap->set, &set, sizeof(set));
	if (error) {
		td->td_retval[0] = error;
		return (0);
	}

	error = kern_sigtimedwait(td, set, &ksi, NULL);
	if (error != 0) {
		if (error == EINTR && td->td_proc->p_osrel < P_OSREL_SIGWAIT)
			error = ERESTART;
		if (error == ERESTART)
			return (error);
		td->td_retval[0] = error;
		return (0);
	}

	error = copyout_c(&ksi.ksi_signo, uap->sig, sizeof(ksi.ksi_signo));
	td->td_retval[0] = error;
	return (0);
}

int
cheriabi_sigtimedwait(struct thread *td, struct cheriabi_sigtimedwait_args *uap)
{
	struct timespec ts;
	struct timespec *timeout;
	sigset_t set;
	ksiginfo_t ksi;
	struct siginfo_c si_c;
	int error;

	if (uap->timeout) {
		error = copyin_c(uap->timeout, &ts, sizeof(ts));
		if (error)
			return (error);
		timeout = &ts;
	} else
		timeout = NULL;

	error = copyin_c(uap->set, &set, sizeof(set));
	if (error)
		return (error);

	error = kern_sigtimedwait(td, set, &ksi, timeout);
	if (error)
		return (error);

	if (uap->info != NULL) {
		siginfo_to_siginfo_c(&ksi.ksi_info, &si_c);
		error = copyout_c(&si_c, uap->info, sizeof(struct siginfo_c));
	}

	if (error == 0)
		td->td_retval[0] = ksi.ksi_signo;
	return (error);
}

int
cheriabi_sigwaitinfo(struct thread *td, struct cheriabi_sigwaitinfo_args *uap)
{
	ksiginfo_t ksi;
	struct siginfo_c si_c;
	sigset_t set;
	int error;

	error = copyin_c(uap->set, &set, sizeof(set));
	if (error)
		return (error);

	error = kern_sigtimedwait(td, set, &ksi, NULL);
	if (error)
		return (error);

	if (uap->info) {
		siginfo_to_siginfo_c(&ksi.ksi_info, &si_c);
		error = copyout_c(&si_c, uap->info, sizeof(struct siginfo_c));
	}
	if (error == 0)
		td->td_retval[0] = ksi.ksi_signo;
	return (error);
}

int
cheriabi_sigpending(struct thread *td, struct cheriabi_sigpending_args *uap)
{
	struct proc *p = td->td_proc;
	sigset_t pending;

	PROC_LOCK(p);
	pending = p->p_sigqueue.sq_signals;
	SIGSETOR(pending, td->td_sigqueue.sq_signals);
	PROC_UNLOCK(p);
	return (copyout_c(&pending, uap->set, sizeof(sigset_t)));
}

int
cheriabi_sigsuspend(struct thread *td, struct cheriabi_sigsuspend_args *uap)
{
	sigset_t mask;
	int error;

	error = copyin_c(uap->sigmask, &mask, sizeof(mask));
	if (error)
		return (error);
	return (kern_sigsuspend(td, mask));
}

int
cheriabi_sigaltstack(struct thread *td,
    struct cheriabi_sigaltstack_args *uap)
{
	void * __capability old_ss_sp;
	struct sigaltstack_c s_c;
	struct sigaltstack ss, oss, *ssp;
	int error;

	if (uap->ss != NULL) {
		error = copyincap_c(uap->ss, &s_c, sizeof(s_c));
		if (error)
			return (error);
		CP(s_c, ss, ss_size);
		CP(s_c, ss, ss_flags);
		/* XXX-BD: what perms to enforce? */
		error = cheriabi_cap_to_ptr((caddr_t *)&ss.ss_sp, s_c.ss_sp,
		    s_c.ss_size, CHERI_PERM_GLOBAL, 1);
		if (error)
			return (error);
		ssp = &ss;
	} else
		ssp = NULL;
	error = kern_sigaltstack(td, ssp, &oss);
	if (error == 0) {
		cheriabi_get_signal_stack_capability(td, &old_ss_sp);
		if (uap->ss != NULL) {
			/*
			 * Install the new signal capability or restore the
			 * thread's default one.
			 */
			cheriabi_set_signal_stack_capability(td,
			    (ss.ss_flags & SS_DISABLE) ? NULL : &s_c.ss_sp);
		}
		if (uap->oss != NULL) {
			s_c.ss_sp = old_ss_sp;
			CP(oss, s_c, ss_size);
			CP(oss, s_c, ss_flags);
			error = copyoutcap_c(&s_c, uap->oss, sizeof(s_c));
		}
	}
	return (error);
}
