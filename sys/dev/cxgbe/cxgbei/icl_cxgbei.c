/*-
 * Copyright (c) 2012 The FreeBSD Foundation
 * Copyright (c) 2015 Chelsio Communications, Inc.
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
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
 *
 */

/*
 * cxgbei implementation of iSCSI Common Layer kobj(9) interface.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"

#ifdef TCP_OFFLOAD
#include <sys/param.h>
#include <sys/capsicum.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>
#include <sys/uio.h>
#include <machine/bus.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/toecore.h>

#include <dev/iscsi/icl.h>
#include <dev/iscsi/iscsi_proto.h>
#include <icl_conn_if.h>

#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_da.h>
#include <cam/ctl/ctl_io.h>
#include <cam/ctl/ctl.h>
#include <cam/ctl/ctl_backend.h>
#include <cam/ctl/ctl_error.h>
#include <cam/ctl/ctl_frontend.h>
#include <cam/ctl/ctl_debug.h>
#include <cam/ctl/ctl_ha.h>
#include <cam/ctl/ctl_ioctl.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/cam_xpt.h>
#include <cam/cam_debug.h>
#include <cam/cam_sim.h>
#include <cam/cam_xpt_sim.h>
#include <cam/cam_xpt_periph.h>
#include <cam/cam_periph.h>
#include <cam/cam_compat.h>
#include <cam/scsi/scsi_message.h>

#include "common/common.h"
#include "common/t4_tcb.h"
#include "tom/t4_tom.h"
#include "cxgbei.h"

/*
 * Use the page pod tag for the TT hash.
 */
#define	TT_HASH(icc, tt)	(G_PPOD_TAG(tt) & (icc)->cmp_hash_mask)

struct cxgbei_ddp_state {
	struct ppod_reservation prsv;
	struct cxgbei_cmp cmp;
};

static MALLOC_DEFINE(M_CXGBEI, "cxgbei", "cxgbei(4)");

SYSCTL_NODE(_kern_icl, OID_AUTO, cxgbei, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Chelsio iSCSI offload");
static int first_burst_length = 8192;
SYSCTL_INT(_kern_icl_cxgbei, OID_AUTO, first_burst_length, CTLFLAG_RWTUN,
    &first_burst_length, 0, "First burst length");
static int max_burst_length = 2 * 1024 * 1024;
SYSCTL_INT(_kern_icl_cxgbei, OID_AUTO, max_burst_length, CTLFLAG_RWTUN,
    &max_burst_length, 0, "Maximum burst length");
static int sendspace = 1048576;
SYSCTL_INT(_kern_icl_cxgbei, OID_AUTO, sendspace, CTLFLAG_RWTUN,
    &sendspace, 0, "Default send socket buffer size");
static int recvspace = 1048576;
SYSCTL_INT(_kern_icl_cxgbei, OID_AUTO, recvspace, CTLFLAG_RWTUN,
    &recvspace, 0, "Default receive socket buffer size");

static volatile u_int icl_cxgbei_ncons;

#define ICL_CONN_LOCK(X)		mtx_lock(X->ic_lock)
#define ICL_CONN_UNLOCK(X)		mtx_unlock(X->ic_lock)
#define ICL_CONN_LOCK_ASSERT(X)		mtx_assert(X->ic_lock, MA_OWNED)
#define ICL_CONN_LOCK_ASSERT_NOT(X)	mtx_assert(X->ic_lock, MA_NOTOWNED)

static icl_conn_new_pdu_t	icl_cxgbei_conn_new_pdu;
static icl_conn_pdu_data_segment_length_t
				    icl_cxgbei_conn_pdu_data_segment_length;
static icl_conn_pdu_append_data_t	icl_cxgbei_conn_pdu_append_data;
static icl_conn_pdu_get_data_t	icl_cxgbei_conn_pdu_get_data;
static icl_conn_pdu_queue_t	icl_cxgbei_conn_pdu_queue;
static icl_conn_pdu_queue_cb_t	icl_cxgbei_conn_pdu_queue_cb;
static icl_conn_handoff_t	icl_cxgbei_conn_handoff;
static icl_conn_free_t		icl_cxgbei_conn_free;
static icl_conn_close_t		icl_cxgbei_conn_close;
static icl_conn_task_setup_t	icl_cxgbei_conn_task_setup;
static icl_conn_task_done_t	icl_cxgbei_conn_task_done;
static icl_conn_transfer_setup_t	icl_cxgbei_conn_transfer_setup;
static icl_conn_transfer_done_t	icl_cxgbei_conn_transfer_done;

static kobj_method_t icl_cxgbei_methods[] = {
	KOBJMETHOD(icl_conn_new_pdu, icl_cxgbei_conn_new_pdu),
	KOBJMETHOD(icl_conn_pdu_free, icl_cxgbei_conn_pdu_free),
	KOBJMETHOD(icl_conn_pdu_data_segment_length,
	    icl_cxgbei_conn_pdu_data_segment_length),
	KOBJMETHOD(icl_conn_pdu_append_data, icl_cxgbei_conn_pdu_append_data),
	KOBJMETHOD(icl_conn_pdu_get_data, icl_cxgbei_conn_pdu_get_data),
	KOBJMETHOD(icl_conn_pdu_queue, icl_cxgbei_conn_pdu_queue),
	KOBJMETHOD(icl_conn_pdu_queue_cb, icl_cxgbei_conn_pdu_queue_cb),
	KOBJMETHOD(icl_conn_handoff, icl_cxgbei_conn_handoff),
	KOBJMETHOD(icl_conn_free, icl_cxgbei_conn_free),
	KOBJMETHOD(icl_conn_close, icl_cxgbei_conn_close),
	KOBJMETHOD(icl_conn_task_setup, icl_cxgbei_conn_task_setup),
	KOBJMETHOD(icl_conn_task_done, icl_cxgbei_conn_task_done),
	KOBJMETHOD(icl_conn_transfer_setup, icl_cxgbei_conn_transfer_setup),
	KOBJMETHOD(icl_conn_transfer_done, icl_cxgbei_conn_transfer_done),
	{ 0, 0 }
};

DEFINE_CLASS(icl_cxgbei, icl_cxgbei_methods, sizeof(struct icl_cxgbei_conn));

void
icl_cxgbei_conn_pdu_free(struct icl_conn *ic, struct icl_pdu *ip)
{
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);

	KASSERT(icp->ref_cnt != 0, ("freeing deleted PDU"));
	MPASS(icp->icp_signature == CXGBEI_PDU_SIGNATURE);
	MPASS(ic == ip->ip_conn);

	m_freem(ip->ip_ahs_mbuf);
	m_freem(ip->ip_data_mbuf);
	m_freem(ip->ip_bhs_mbuf);

	KASSERT(ic != NULL || icp->ref_cnt == 1,
	    ("orphaned PDU has oustanding references"));

	if (atomic_fetchadd_int(&icp->ref_cnt, -1) != 1)
		return;

	free(icp, M_CXGBEI);
#ifdef DIAGNOSTIC
	if (__predict_true(ic != NULL))
		refcount_release(&ic->ic_outstanding_pdus);
#endif
}

static void
icl_cxgbei_pdu_call_cb(struct icl_pdu *ip)
{
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);

	MPASS(icp->icp_signature == CXGBEI_PDU_SIGNATURE);

	if (icp->cb != NULL)
		icp->cb(ip, icp->error);
#ifdef DIAGNOSTIC
	if (__predict_true(ip->ip_conn != NULL))
		refcount_release(&ip->ip_conn->ic_outstanding_pdus);
#endif
	free(icp, M_CXGBEI);
}

static void
icl_cxgbei_pdu_done(struct icl_pdu *ip, int error)
{
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);

	if (error != 0)
		icp->error = error;

	m_freem(ip->ip_ahs_mbuf);
	ip->ip_ahs_mbuf = NULL;
	m_freem(ip->ip_data_mbuf);
	ip->ip_data_mbuf = NULL;
	m_freem(ip->ip_bhs_mbuf);
	ip->ip_bhs_mbuf = NULL;

	/*
	 * All other references to this PDU should have been dropped
	 * by the m_freem() of ip_data_mbuf.
	 */
	if (atomic_fetchadd_int(&icp->ref_cnt, -1) == 1)
		icl_cxgbei_pdu_call_cb(ip);
	else
		__assert_unreachable();
}

static void
icl_cxgbei_mbuf_done(struct mbuf *mb)
{

	struct icl_cxgbei_pdu *icp = (struct icl_cxgbei_pdu *)mb->m_ext.ext_arg1;

	/*
	 * NB: mb_free_mext() might leave ref_cnt as 1 without
	 * decrementing it if it hits the fast path in the ref_cnt
	 * check.
	 */
	icl_cxgbei_pdu_call_cb(&icp->ip);
}

struct icl_pdu *
icl_cxgbei_new_pdu(int flags)
{
	struct icl_cxgbei_pdu *icp;
	struct icl_pdu *ip;
	struct mbuf *m;

	icp = malloc(sizeof(*icp), M_CXGBEI, flags | M_ZERO);
	if (__predict_false(icp == NULL))
		return (NULL);

	icp->icp_signature = CXGBEI_PDU_SIGNATURE;
	icp->ref_cnt = 1;
	ip = &icp->ip;

	m = m_gethdr(flags, MT_DATA);
	if (__predict_false(m == NULL)) {
		free(icp, M_CXGBEI);
		return (NULL);
	}

	ip->ip_bhs_mbuf = m;
	ip->ip_bhs = mtod(m, struct iscsi_bhs *);
	memset(ip->ip_bhs, 0, sizeof(*ip->ip_bhs));
	m->m_len = sizeof(struct iscsi_bhs);
	m->m_pkthdr.len = m->m_len;

	return (ip);
}

void
icl_cxgbei_new_pdu_set_conn(struct icl_pdu *ip, struct icl_conn *ic)
{

	ip->ip_conn = ic;
#ifdef DIAGNOSTIC
	refcount_acquire(&ic->ic_outstanding_pdus);
#endif
}

/*
 * Allocate icl_pdu with empty BHS to fill up by the caller.
 */
static struct icl_pdu *
icl_cxgbei_conn_new_pdu(struct icl_conn *ic, int flags)
{
	struct icl_pdu *ip;

	ip = icl_cxgbei_new_pdu(flags);
	if (__predict_false(ip == NULL))
		return (NULL);
	icl_cxgbei_new_pdu_set_conn(ip, ic);

	return (ip);
}

static size_t
icl_pdu_data_segment_length(const struct icl_pdu *request)
{
	uint32_t len = 0;

	len += request->ip_bhs->bhs_data_segment_len[0];
	len <<= 8;
	len += request->ip_bhs->bhs_data_segment_len[1];
	len <<= 8;
	len += request->ip_bhs->bhs_data_segment_len[2];

	return (len);
}

size_t
icl_cxgbei_conn_pdu_data_segment_length(struct icl_conn *ic,
    const struct icl_pdu *request)
{

	return (icl_pdu_data_segment_length(request));
}

static struct mbuf *
finalize_pdu(struct icl_cxgbei_conn *icc, struct icl_cxgbei_pdu *icp)
{
	struct icl_pdu *ip = &icp->ip;
	uint8_t ulp_submode, padding;
	struct mbuf *m, *last;
	struct iscsi_bhs *bhs;
	int data_len;

	/*
	 * Fix up the data segment mbuf first.
	 */
	m = ip->ip_data_mbuf;
	ulp_submode = icc->ulp_submode;
	if (m != NULL) {
		last = m_last(m);

		/*
		 * Round up the data segment to a 4B boundary.	Pad with 0 if
		 * necessary.  There will definitely be room in the mbuf.
		 */
		padding = roundup2(ip->ip_data_len, 4) - ip->ip_data_len;
		if (padding != 0) {
			MPASS(padding <= M_TRAILINGSPACE(last));
			bzero(mtod(last, uint8_t *) + last->m_len, padding);
			last->m_len += padding;
		}
	} else {
		MPASS(ip->ip_data_len == 0);
		ulp_submode &= ~ULP_CRC_DATA;
		padding = 0;
	}

	/*
	 * Now the header mbuf that has the BHS.
	 */
	m = ip->ip_bhs_mbuf;
	MPASS(m->m_pkthdr.len == sizeof(struct iscsi_bhs));
	MPASS(m->m_len == sizeof(struct iscsi_bhs));

	bhs = ip->ip_bhs;
	data_len = ip->ip_data_len;
	if (data_len > icc->ic.ic_max_send_data_segment_length) {
		struct iscsi_bhs_data_in *bhsdi;
		int flags;

		KASSERT(padding == 0, ("%s: ISO with padding %d for icp %p",
		    __func__, padding, icp));
		switch (bhs->bhs_opcode) {
		case ISCSI_BHS_OPCODE_SCSI_DATA_OUT:
			flags = 1;
			break;
		case ISCSI_BHS_OPCODE_SCSI_DATA_IN:
			flags = 2;
			break;
		default:
			panic("invalid opcode %#x for ISO", bhs->bhs_opcode);
		}
		data_len = icc->ic.ic_max_send_data_segment_length;
		bhsdi = (struct iscsi_bhs_data_in *)bhs;
		if (bhsdi->bhsdi_flags & BHSDI_FLAGS_F) {
			/*
			 * Firmware will set F on the final PDU in the
			 * burst.
			 */
			flags |= CXGBE_ISO_F;
			bhsdi->bhsdi_flags &= ~BHSDI_FLAGS_F;
		}
		set_mbuf_iscsi_iso(m, true);
		set_mbuf_iscsi_iso_flags(m, flags);
		set_mbuf_iscsi_iso_mss(m, data_len);
	}

	bhs->bhs_data_segment_len[2] = data_len;
	bhs->bhs_data_segment_len[1] = data_len >> 8;
	bhs->bhs_data_segment_len[0] = data_len >> 16;

	/*
	 * Extract mbuf chain from PDU.
	 */
	m->m_pkthdr.len += ip->ip_data_len + padding;
	m->m_next = ip->ip_data_mbuf;
	set_mbuf_ulp_submode(m, ulp_submode);
	ip->ip_bhs_mbuf = NULL;
	ip->ip_data_mbuf = NULL;
	ip->ip_bhs = NULL;

	/*
	 * Drop PDU reference on icp.  Additional references might
	 * still be held by zero-copy PDU buffers (ICL_NOCOPY).
	 */
	if (atomic_fetchadd_int(&icp->ref_cnt, -1) == 1)
		icl_cxgbei_pdu_call_cb(ip);

	return (m);
}

int
icl_cxgbei_conn_pdu_append_data(struct icl_conn *ic, struct icl_pdu *ip,
    const void *addr, size_t len, int flags)
{
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);
	struct mbuf *m, *m_tail;
	const char *src;

	MPASS(icp->icp_signature == CXGBEI_PDU_SIGNATURE);
	MPASS(ic == ip->ip_conn);
	KASSERT(len > 0, ("%s: len is %jd", __func__, (intmax_t)len));

	m_tail = ip->ip_data_mbuf;
	if (m_tail != NULL)
		for (; m_tail->m_next != NULL; m_tail = m_tail->m_next)
			;

	if (flags & ICL_NOCOPY) {
		m = m_get(flags & ~ICL_NOCOPY, MT_DATA);
		if (m == NULL) {
			ICL_WARN("failed to allocate mbuf");
			return (ENOMEM);
		}

		m->m_flags |= M_RDONLY;
		m_extaddref(m, __DECONST(char *, addr), len, &icp->ref_cnt,
		    icl_cxgbei_mbuf_done, icp, NULL);
		m->m_len = len;
		if (ip->ip_data_mbuf == NULL) {
			ip->ip_data_mbuf = m;
			ip->ip_data_len = len;
		} else {
			m_tail->m_next = m;
			m_tail = m_tail->m_next;
			ip->ip_data_len += len;
		}

		return (0);
	}

	src = (const char *)addr;

	/* Allocate as jumbo mbufs of size MJUM16BYTES. */
	while (len >= MJUM16BYTES) {
		m = m_getjcl(M_NOWAIT, MT_DATA, 0, MJUM16BYTES);
		if (__predict_false(m == NULL)) {
			if ((flags & M_WAITOK) != 0) {
				/* Fall back to non-jumbo mbufs. */
				break;
			}
			return (ENOMEM);
		}
		memcpy(mtod(m, void *), src, MJUM16BYTES);
		m->m_len = MJUM16BYTES;
		if (ip->ip_data_mbuf == NULL) {
			ip->ip_data_mbuf = m_tail = m;
			ip->ip_data_len = MJUM16BYTES;
		} else {
			m_tail->m_next = m;
			m_tail = m_tail->m_next;
			ip->ip_data_len += MJUM16BYTES;
		}
		src += MJUM16BYTES;
		len -= MJUM16BYTES;
	}

	/* Allocate mbuf chain for the remaining data. */
	if (len != 0) {
		m = m_getm2(NULL, len, flags, MT_DATA, 0);
		if (__predict_false(m == NULL))
			return (ENOMEM);
		if (ip->ip_data_mbuf == NULL) {
			ip->ip_data_mbuf = m;
			ip->ip_data_len = len;
		} else {
			m_tail->m_next = m;
			ip->ip_data_len += len;
		}
		for (; m != NULL; m = m->m_next) {
			m->m_len = min(len, M_SIZE(m));
			memcpy(mtod(m, void *), src, m->m_len);
			src += m->m_len;
			len -= m->m_len;
		}
		MPASS(len == 0);
	}
	MPASS(ip->ip_data_len <= max(ic->ic_max_send_data_segment_length,
	    ic->ic_hw_isomax));

	return (0);
}

void
icl_cxgbei_conn_pdu_get_data(struct icl_conn *ic, struct icl_pdu *ip,
    size_t off, void *addr, size_t len)
{
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);

	if (icp->icp_flags & ICPF_RX_DDP)
		return; /* data is DDP'ed, no need to copy */
	m_copydata(ip->ip_data_mbuf, off, len, addr);
}

void
icl_cxgbei_conn_pdu_queue(struct icl_conn *ic, struct icl_pdu *ip)
{
	icl_cxgbei_conn_pdu_queue_cb(ic, ip, NULL);
}

void
icl_cxgbei_conn_pdu_queue_cb(struct icl_conn *ic, struct icl_pdu *ip,
			     icl_pdu_cb cb)
{
	struct epoch_tracker et;
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);
	struct icl_cxgbei_pdu *icp = ip_to_icp(ip);
	struct socket *so = ic->ic_socket;
	struct toepcb *toep = icc->toep;
	struct inpcb *inp;
	struct mbuf *m;

	MPASS(ic == ip->ip_conn);
	MPASS(ip->ip_bhs_mbuf != NULL);
	/* The kernel doesn't generate PDUs with AHS. */
	MPASS(ip->ip_ahs_mbuf == NULL && ip->ip_ahs_len == 0);

	ICL_CONN_LOCK_ASSERT(ic);

	icp->cb = cb;

	/* NOTE: sowriteable without so_snd lock is a mostly harmless race. */
	if (ic->ic_disconnecting || so == NULL || !sowriteable(so)) {
		icl_cxgbei_pdu_done(ip, ENOTCONN);
		return;
	}

	m = finalize_pdu(icc, icp);
	M_ASSERTPKTHDR(m);
	MPASS((m->m_pkthdr.len & 3) == 0);

	/*
	 * Do not get inp from toep->inp as the toepcb might have detached
	 * already.
	 */
	inp = sotoinpcb(so);
	CURVNET_SET(toep->vnet);
	NET_EPOCH_ENTER(et);
	INP_WLOCK(inp);
	if (__predict_false(inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) ||
	    __predict_false((toep->flags & TPF_ATTACHED) == 0))
		m_freem(m);
	else {
		mbufq_enqueue(&toep->ulp_pduq, m);
		t4_push_pdus(icc->sc, toep, 0);
	}
	INP_WUNLOCK(inp);
	NET_EPOCH_EXIT(et);
	CURVNET_RESTORE();
}

static struct icl_conn *
icl_cxgbei_new_conn(const char *name, struct mtx *lock)
{
	struct icl_cxgbei_conn *icc;
	struct icl_conn *ic;

	refcount_acquire(&icl_cxgbei_ncons);

	icc = (struct icl_cxgbei_conn *)kobj_create(&icl_cxgbei_class, M_CXGBE,
	    M_WAITOK | M_ZERO);
	icc->icc_signature = CXGBEI_CONN_SIGNATURE;
	STAILQ_INIT(&icc->rcvd_pdus);

	icc->cmp_table = hashinit(64, M_CXGBEI, &icc->cmp_hash_mask);
	mtx_init(&icc->cmp_lock, "cxgbei_cmp", NULL, MTX_DEF);

	ic = &icc->ic;
	ic->ic_lock = lock;

#ifdef DIAGNOSTIC
	refcount_init(&ic->ic_outstanding_pdus, 0);
#endif
	ic->ic_name = name;
	ic->ic_offload = "cxgbei";
	ic->ic_unmapped = false;

	CTR2(KTR_CXGBE, "%s: icc %p", __func__, icc);

	return (ic);
}

void
icl_cxgbei_conn_free(struct icl_conn *ic)
{
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);

	MPASS(icc->icc_signature == CXGBEI_CONN_SIGNATURE);

	CTR2(KTR_CXGBE, "%s: icc %p", __func__, icc);

	mtx_destroy(&icc->cmp_lock);
	hashdestroy(icc->cmp_table, M_CXGBEI, icc->cmp_hash_mask);
	kobj_delete((struct kobj *)icc, M_CXGBE);
	refcount_release(&icl_cxgbei_ncons);
}

static int
icl_cxgbei_setsockopt(struct icl_conn *ic, struct socket *so, int sspace,
    int rspace)
{
	struct sockopt opt;
	int error, one = 1, ss, rs;

	ss = max(sendspace, sspace);
	rs = max(recvspace, rspace);

	error = soreserve(so, ss, rs);
	if (error != 0) {
		icl_cxgbei_conn_close(ic);
		return (error);
	}
	SOCKBUF_LOCK(&so->so_snd);
	so->so_snd.sb_flags |= SB_AUTOSIZE;
	SOCKBUF_UNLOCK(&so->so_snd);
	SOCKBUF_LOCK(&so->so_rcv);
	so->so_rcv.sb_flags |= SB_AUTOSIZE;
	SOCKBUF_UNLOCK(&so->so_rcv);

	/*
	 * Disable Nagle.
	 */
	bzero(&opt, sizeof(opt));
	opt.sopt_dir = SOPT_SET;
	opt.sopt_level = IPPROTO_TCP;
	opt.sopt_name = TCP_NODELAY;
	opt.sopt_val = &one;
	opt.sopt_valsize = sizeof(one);
	error = sosetopt(so, &opt);
	if (error != 0) {
		icl_cxgbei_conn_close(ic);
		return (error);
	}

	return (0);
}

/*
 * Request/response structure used to find out the adapter offloading a socket.
 */
struct find_ofld_adapter_rr {
	struct socket *so;
	struct adapter *sc;	/* result */
};

static void
find_offload_adapter(struct adapter *sc, void *arg)
{
	struct find_ofld_adapter_rr *fa = arg;
	struct socket *so = fa->so;
	struct tom_data *td = sc->tom_softc;
	struct tcpcb *tp;
	struct inpcb *inp;

	/* Non-TCP were filtered out earlier. */
	MPASS(so->so_proto->pr_protocol == IPPROTO_TCP);

	if (fa->sc != NULL)
		return;	/* Found already. */

	if (td == NULL)
		return;	/* TOE not enabled on this adapter. */

	inp = sotoinpcb(so);
	INP_WLOCK(inp);
	if ((inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) == 0) {
		tp = intotcpcb(inp);
		if (tp->t_flags & TF_TOE && tp->tod == &td->tod)
			fa->sc = sc;	/* Found. */
	}
	INP_WUNLOCK(inp);
}

/* XXXNP: move this to t4_tom. */
static void
send_iscsi_flowc_wr(struct adapter *sc, struct toepcb *toep, int maxlen)
{
	struct wrqe *wr;
	struct fw_flowc_wr *flowc;
	const u_int nparams = 1;
	u_int flowclen;
	struct ofld_tx_sdesc *txsd = &toep->txsd[toep->txsd_pidx];

	flowclen = sizeof(*flowc) + nparams * sizeof(struct fw_flowc_mnemval);

	wr = alloc_wrqe(roundup2(flowclen, 16), &toep->ofld_txq->wrq);
	if (wr == NULL) {
		/* XXX */
		panic("%s: allocation failure.", __func__);
	}
	flowc = wrtod(wr);
	memset(flowc, 0, wr->wr_len);

	flowc->op_to_nparams = htobe32(V_FW_WR_OP(FW_FLOWC_WR) |
	    V_FW_FLOWC_WR_NPARAMS(nparams));
	flowc->flowid_len16 = htonl(V_FW_WR_LEN16(howmany(flowclen, 16)) |
	    V_FW_WR_FLOWID(toep->tid));

	flowc->mnemval[0].mnemonic = FW_FLOWC_MNEM_TXDATAPLEN_MAX;
	flowc->mnemval[0].val = htobe32(maxlen);

	txsd->tx_credits = howmany(flowclen, 16);
	txsd->plen = 0;
	KASSERT(toep->tx_credits >= txsd->tx_credits && toep->txsd_avail > 0,
	    ("%s: not enough credits (%d)", __func__, toep->tx_credits));
	toep->tx_credits -= txsd->tx_credits;
	if (__predict_false(++toep->txsd_pidx == toep->txsd_total))
		toep->txsd_pidx = 0;
	toep->txsd_avail--;

	t4_wrq_tx(sc, wr);
}

static void
set_ulp_mode_iscsi(struct adapter *sc, struct toepcb *toep, u_int ulp_submode)
{
	uint64_t val;

	CTR3(KTR_CXGBE, "%s: tid %u, ULP_MODE_ISCSI, submode=%#x",
	    __func__, toep->tid, ulp_submode);

	val = V_TCB_ULP_TYPE(ULP_MODE_ISCSI) | V_TCB_ULP_RAW(ulp_submode);
	t4_set_tcb_field(sc, toep->ctrlq, toep, W_TCB_ULP_TYPE,
	    V_TCB_ULP_TYPE(M_TCB_ULP_TYPE) | V_TCB_ULP_RAW(M_TCB_ULP_RAW), val,
	    0, 0);

	val = V_TF_RX_FLOW_CONTROL_DISABLE(1ULL);
	t4_set_tcb_field(sc, toep->ctrlq, toep, W_TCB_T_FLAGS, val, val, 0, 0);
}

/*
 * XXXNP: Who is responsible for cleaning up the socket if this returns with an
 * error?  Review all error paths.
 *
 * XXXNP: What happens to the socket's fd reference if the operation is
 * successful, and how does that affect the socket's life cycle?
 */
int
icl_cxgbei_conn_handoff(struct icl_conn *ic, int fd)
{
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);
	struct cxgbei_data *ci;
	struct find_ofld_adapter_rr fa;
	struct file *fp;
	struct socket *so;
	struct inpcb *inp;
	struct tcpcb *tp;
	struct toepcb *toep;
	cap_rights_t rights;
	int error, max_iso_pdus;

	MPASS(icc->icc_signature == CXGBEI_CONN_SIGNATURE);
	ICL_CONN_LOCK_ASSERT_NOT(ic);

	/*
	 * Steal the socket from userland.
	 */
	error = fget(curthread, fd,
	    cap_rights_init_one(&rights, CAP_SOCK_CLIENT), &fp);
	if (error != 0)
		return (error);
	if (fp->f_type != DTYPE_SOCKET) {
		fdrop(fp, curthread);
		return (EINVAL);
	}
	so = fp->f_data;
	if (so->so_type != SOCK_STREAM ||
	    so->so_proto->pr_protocol != IPPROTO_TCP) {
		fdrop(fp, curthread);
		return (EINVAL);
	}

	ICL_CONN_LOCK(ic);
	if (ic->ic_socket != NULL) {
		ICL_CONN_UNLOCK(ic);
		fdrop(fp, curthread);
		return (EBUSY);
	}
	ic->ic_disconnecting = false;
	ic->ic_socket = so;
	fp->f_ops = &badfileops;
	fp->f_data = NULL;
	fdrop(fp, curthread);
	ICL_CONN_UNLOCK(ic);

	/* Find the adapter offloading this socket. */
	fa.sc = NULL;
	fa.so = so;
	t4_iterate(find_offload_adapter, &fa);
	if (fa.sc == NULL)
		return (EINVAL);
	icc->sc = fa.sc;
	ci = icc->sc->iscsi_ulp_softc;

	inp = sotoinpcb(so);
	INP_WLOCK(inp);
	tp = intotcpcb(inp);
	if (inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT))
		error = EBUSY;
	else {
		/*
		 * socket could not have been "unoffloaded" if here.
		 */
		MPASS(tp->t_flags & TF_TOE);
		MPASS(tp->tod != NULL);
		MPASS(tp->t_toe != NULL);
		toep = tp->t_toe;
		MPASS(toep->vi->adapter == icc->sc);
		icc->toep = toep;
		icc->cwt = cxgbei_select_worker_thread(icc);

		icc->ulp_submode = 0;
		if (ic->ic_header_crc32c)
			icc->ulp_submode |= ULP_CRC_HEADER;
		if (ic->ic_data_crc32c)
			icc->ulp_submode |= ULP_CRC_DATA;

		if (icc->sc->tt.iso && chip_id(icc->sc) >= CHELSIO_T5) {
			max_iso_pdus = CXGBEI_MAX_ISO_PAYLOAD /
			    ci->max_tx_pdu_len;
			ic->ic_hw_isomax = max_iso_pdus *
			    ic->ic_max_send_data_segment_length;
		} else
			max_iso_pdus = 1;

		so->so_options |= SO_NO_DDP;
		toep->params.ulp_mode = ULP_MODE_ISCSI;
		toep->ulpcb = icc;

		send_iscsi_flowc_wr(icc->sc, toep,
		    roundup(max_iso_pdus * ci->max_tx_pdu_len, tp->t_maxseg));
		set_ulp_mode_iscsi(icc->sc, toep, icc->ulp_submode);
		error = 0;
	}
	INP_WUNLOCK(inp);

	if (error == 0) {
		error = icl_cxgbei_setsockopt(ic, so, ci->max_tx_pdu_len,
		    ci->max_rx_pdu_len);
	}

	return (error);
}

void
icl_cxgbei_conn_close(struct icl_conn *ic)
{
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);
	struct icl_pdu *ip;
	struct socket *so;
	struct sockbuf *sb;
	struct inpcb *inp;
	struct toepcb *toep = icc->toep;

	MPASS(icc->icc_signature == CXGBEI_CONN_SIGNATURE);
	ICL_CONN_LOCK_ASSERT_NOT(ic);

	ICL_CONN_LOCK(ic);
	so = ic->ic_socket;
	if (ic->ic_disconnecting || so == NULL) {
		CTR4(KTR_CXGBE, "%s: icc %p (disconnecting = %d), so %p",
		    __func__, icc, ic->ic_disconnecting, so);
		ICL_CONN_UNLOCK(ic);
		return;
	}
	ic->ic_disconnecting = true;

#ifdef DIAGNOSTIC
	KASSERT(ic->ic_outstanding_pdus == 0,
	    ("destroying session with %d outstanding PDUs",
	     ic->ic_outstanding_pdus));
#endif
	ICL_CONN_UNLOCK(ic);

	CTR3(KTR_CXGBE, "%s: tid %d, icc %p", __func__, toep ? toep->tid : -1,
	    icc);
	inp = sotoinpcb(so);
	sb = &so->so_rcv;
	INP_WLOCK(inp);
	if (toep != NULL) {	/* NULL if connection was never offloaded. */
		toep->ulpcb = NULL;

		/* Discard PDUs queued for TX. */
		mbufq_drain(&toep->ulp_pduq);

		/*
		 * Wait for the cwt threads to stop processing this
		 * connection.
		 */
		SOCKBUF_LOCK(sb);
		if (icc->rx_flags & RXF_ACTIVE) {
			volatile u_int *p = &icc->rx_flags;

			SOCKBUF_UNLOCK(sb);
			INP_WUNLOCK(inp);

			while (*p & RXF_ACTIVE)
				pause("conclo", 1);

			INP_WLOCK(inp);
			SOCKBUF_LOCK(sb);
		}

		/*
		 * Discard received PDUs not passed to the iSCSI
		 * layer.
		 */
		while (!STAILQ_EMPTY(&icc->rcvd_pdus)) {
			ip = STAILQ_FIRST(&icc->rcvd_pdus);
			STAILQ_REMOVE_HEAD(&icc->rcvd_pdus, ip_next);
			icl_cxgbei_pdu_done(ip, ENOTCONN);
		}
		SOCKBUF_UNLOCK(sb);
	}
	INP_WUNLOCK(inp);

	ICL_CONN_LOCK(ic);
	ic->ic_socket = NULL;
	ICL_CONN_UNLOCK(ic);

	/*
	 * XXXNP: we should send RST instead of FIN when PDUs held in various
	 * queues were purged instead of delivered reliably but soabort isn't
	 * really general purpose and wouldn't do the right thing here.
	 */
	soref(so);
	soclose(so);

	/*
	 * Wait for the socket to fully close.  This ensures any
	 * pending received data has been received (and in particular,
	 * any data that would be received by DDP has been handled).
	 * Callers assume that it is safe to free buffers for tasks
	 * and transfers after this function returns.
	 */
	SOCK_LOCK(so);
	while ((so->so_state & SS_ISDISCONNECTED) == 0)
		mtx_sleep(&so->so_timeo, SOCK_MTX(so), PSOCK, "conclo2", 0);
	CURVNET_SET(so->so_vnet);
	sorele(so);
	CURVNET_RESTORE();
}

static void
cxgbei_insert_cmp(struct icl_cxgbei_conn *icc, struct cxgbei_cmp *cmp,
    uint32_t tt)
{
#ifdef INVARIANTS
	struct cxgbei_cmp *cmp2;
#endif

	cmp->tt = tt;

	mtx_lock(&icc->cmp_lock);
#ifdef INVARIANTS
	LIST_FOREACH(cmp2, &icc->cmp_table[TT_HASH(icc, tt)], link) {
		KASSERT(cmp2->tt != tt, ("%s: duplicate cmp", __func__));
	}
#endif
	LIST_INSERT_HEAD(&icc->cmp_table[TT_HASH(icc, tt)], cmp, link);
	mtx_unlock(&icc->cmp_lock);
}

struct cxgbei_cmp *
cxgbei_find_cmp(struct icl_cxgbei_conn *icc, uint32_t tt)
{
	struct cxgbei_cmp *cmp;

	mtx_lock(&icc->cmp_lock);
	LIST_FOREACH(cmp, &icc->cmp_table[TT_HASH(icc, tt)], link) {
		if (cmp->tt == tt)
			break;
	}
	mtx_unlock(&icc->cmp_lock);
	return (cmp);
}

static void
cxgbei_rm_cmp(struct icl_cxgbei_conn *icc, struct cxgbei_cmp *cmp)
{
#ifdef INVARIANTS
	struct cxgbei_cmp *cmp2;
#endif

	mtx_lock(&icc->cmp_lock);

#ifdef INVARIANTS
	LIST_FOREACH(cmp2, &icc->cmp_table[TT_HASH(icc, cmp->tt)], link) {
		if (cmp2 == cmp)
			goto found;
	}
	panic("%s: could not find cmp", __func__);
found:
#endif
	LIST_REMOVE(cmp, link);
	mtx_unlock(&icc->cmp_lock);
}

int
icl_cxgbei_conn_task_setup(struct icl_conn *ic, struct icl_pdu *ip,
    struct ccb_scsiio *csio, uint32_t *ittp, void **arg)
{
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);
	struct toepcb *toep = icc->toep;
	struct adapter *sc = icc->sc;
	struct cxgbei_data *ci = sc->iscsi_ulp_softc;
	struct ppod_region *pr = &ci->pr;
	struct cxgbei_ddp_state *ddp;
	struct ppod_reservation *prsv;
	struct inpcb *inp;
	struct mbufq mq;
	uint32_t itt;
	int rc = 0;

	ICL_CONN_LOCK_ASSERT(ic);

	/* This is for the offload driver's state.  Must not be set already. */
	MPASS(arg != NULL);
	MPASS(*arg == NULL);

	if (ic->ic_disconnecting || ic->ic_socket == NULL)
		return (ECONNRESET);

	if ((csio->ccb_h.flags & CAM_DIR_MASK) != CAM_DIR_IN ||
	    csio->dxfer_len < ci->ddp_threshold) {
no_ddp:
		/*
		 * No DDP for this I/O.	 Allocate an ITT (based on the one
		 * passed in) that cannot be a valid hardware DDP tag in the
		 * iSCSI region.
		 */
		itt = *ittp & M_PPOD_TAG;
		itt = V_PPOD_TAG(itt) | pr->pr_invalid_bit;
		*ittp = htobe32(itt);
		MPASS(*arg == NULL);	/* State is maintained for DDP only. */
		if (rc != 0)
			counter_u64_add(
			    toep->ofld_rxq->rx_iscsi_ddp_setup_error, 1);
		return (0);
	}

	/*
	 * Reserve resources for DDP, update the itt that should be used in the
	 * PDU, and save DDP specific state for this I/O in *arg.
	 */
	ddp = malloc(sizeof(*ddp), M_CXGBEI, M_NOWAIT | M_ZERO);
	if (ddp == NULL) {
		rc = ENOMEM;
		goto no_ddp;
	}
	prsv = &ddp->prsv;

	/* XXX add support for all CAM_DATA_ types */
	MPASS((csio->ccb_h.flags & CAM_DATA_MASK) == CAM_DATA_VADDR);
	rc = t4_alloc_page_pods_for_buf(pr, (vm_offset_t)csio->data_ptr,
	    csio->dxfer_len, prsv);
	if (rc != 0) {
		free(ddp, M_CXGBEI);
		goto no_ddp;
	}

	mbufq_init(&mq, INT_MAX);
	rc = t4_write_page_pods_for_buf(sc, toep, prsv,
	    (vm_offset_t)csio->data_ptr, csio->dxfer_len, &mq);
	if (__predict_false(rc != 0)) {
		mbufq_drain(&mq);
		t4_free_page_pods(prsv);
		free(ddp, M_CXGBEI);
		goto no_ddp;
	}

	/*
	 * Do not get inp from toep->inp as the toepcb might have
	 * detached already.
	 */
	inp = sotoinpcb(ic->ic_socket);
	INP_WLOCK(inp);
	if ((inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) != 0) {
		INP_WUNLOCK(inp);
		mbufq_drain(&mq);
		t4_free_page_pods(prsv);
		free(ddp, M_CXGBEI);
		return (ECONNRESET);
	}
	mbufq_concat(&toep->ulp_pduq, &mq);
	INP_WUNLOCK(inp);

	ddp->cmp.last_datasn = -1;
	cxgbei_insert_cmp(icc, &ddp->cmp, prsv->prsv_tag);
	*ittp = htobe32(prsv->prsv_tag);
	*arg = prsv;
	counter_u64_add(toep->ofld_rxq->rx_iscsi_ddp_setup_ok, 1);
	return (0);
}

void
icl_cxgbei_conn_task_done(struct icl_conn *ic, void *arg)
{

	if (arg != NULL) {
		struct cxgbei_ddp_state *ddp = arg;

		cxgbei_rm_cmp(ic_to_icc(ic), &ddp->cmp);
		t4_free_page_pods(&ddp->prsv);
		free(ddp, M_CXGBEI);
	}
}

static inline bool
ddp_sgl_check(struct ctl_sg_entry *sg, int entries, int xferlen)
{
	int total_len = 0;

	MPASS(entries > 0);
	if (((vm_offset_t)sg[--entries].addr & 3U) != 0)
		return (false);

	total_len += sg[entries].len;

	while (--entries >= 0) {
		if (((vm_offset_t)sg[entries].addr & PAGE_MASK) != 0 ||
		    (sg[entries].len % PAGE_SIZE) != 0)
			return (false);
		total_len += sg[entries].len;
	}

	MPASS(total_len == xferlen);
	return (true);
}

/* XXXNP: PDU should be passed in as parameter, like on the initiator. */
#define io_to_request_pdu(io) ((io)->io_hdr.ctl_private[CTL_PRIV_FRONTEND].ptr)
#define io_to_ddp_state(io) ((io)->io_hdr.ctl_private[CTL_PRIV_FRONTEND2].ptr)

int
icl_cxgbei_conn_transfer_setup(struct icl_conn *ic, union ctl_io *io,
    uint32_t *tttp, void **arg)
{
	struct icl_cxgbei_conn *icc = ic_to_icc(ic);
	struct toepcb *toep = icc->toep;
	struct ctl_scsiio *ctsio = &io->scsiio;
	struct adapter *sc = icc->sc;
	struct cxgbei_data *ci = sc->iscsi_ulp_softc;
	struct ppod_region *pr = &ci->pr;
	struct cxgbei_ddp_state *ddp;
	struct ppod_reservation *prsv;
	struct ctl_sg_entry *sgl, sg_entry;
	struct inpcb *inp;
	struct mbufq mq;
	int sg_entries = ctsio->kern_sg_entries;
	uint32_t ttt;
	int xferlen, rc = 0, alias;

	/* This is for the offload driver's state.  Must not be set already. */
	MPASS(arg != NULL);
	MPASS(*arg == NULL);

	if (ctsio->ext_data_filled == 0) {
		int first_burst;
		struct icl_pdu *ip = io_to_request_pdu(io);
#ifdef INVARIANTS
		struct icl_cxgbei_pdu *icp = ip_to_icp(ip);

		MPASS(icp->icp_signature == CXGBEI_PDU_SIGNATURE);
		MPASS(ic == ip->ip_conn);
		MPASS(ip->ip_bhs_mbuf != NULL);
#endif
		first_burst = icl_pdu_data_segment_length(ip);

		/*
		 * Note that ICL calls conn_transfer_setup even if the first
		 * burst had everything and there's nothing left to transfer.
		 *
		 * NB: The CTL frontend might have provided a buffer
		 * whose length (kern_data_len) is smaller than the
		 * FirstBurstLength of unsolicited data.  Treat those
		 * as an empty transfer.
		 */
		xferlen = ctsio->kern_data_len;
		if (xferlen < first_burst ||
		    xferlen - first_burst < ci->ddp_threshold) {
no_ddp:
			/*
			 * No DDP for this transfer.  Allocate a TTT (based on
			 * the one passed in) that cannot be a valid hardware
			 * DDP tag in the iSCSI region.
			 */
			ttt = *tttp & M_PPOD_TAG;
			ttt = V_PPOD_TAG(ttt) | pr->pr_invalid_bit;
			*tttp = htobe32(ttt);
			MPASS(io_to_ddp_state(io) == NULL);
			if (rc != 0)
				counter_u64_add(
				    toep->ofld_rxq->rx_iscsi_ddp_setup_error, 1);
			return (0);
		}

		if (sg_entries == 0) {
			sgl = &sg_entry;
			sgl->len = xferlen;
			sgl->addr = (void *)ctsio->kern_data_ptr;
			sg_entries = 1;
		} else
			sgl = (void *)ctsio->kern_data_ptr;

		if (!ddp_sgl_check(sgl, sg_entries, xferlen))
			goto no_ddp;

		/*
		 * Reserve resources for DDP, update the ttt that should be used
		 * in the PDU, and save DDP specific state for this I/O.
		 */
		MPASS(io_to_ddp_state(io) == NULL);
		ddp = malloc(sizeof(*ddp), M_CXGBEI, M_NOWAIT | M_ZERO);
		if (ddp == NULL) {
			rc = ENOMEM;
			goto no_ddp;
		}
		prsv = &ddp->prsv;

		rc = t4_alloc_page_pods_for_sgl(pr, sgl, sg_entries, prsv);
		if (rc != 0) {
			free(ddp, M_CXGBEI);
			goto no_ddp;
		}

		mbufq_init(&mq, INT_MAX);
		rc = t4_write_page_pods_for_sgl(sc, toep, prsv, sgl, sg_entries,
		    xferlen, &mq);
		if (__predict_false(rc != 0)) {
			mbufq_drain(&mq);
			t4_free_page_pods(prsv);
			free(ddp, M_CXGBEI);
			goto no_ddp;
		}

		/*
		 * Do not get inp from toep->inp as the toepcb might
		 * have detached already.
		 */
		ICL_CONN_LOCK(ic);
		if (ic->ic_disconnecting || ic->ic_socket == NULL) {
			ICL_CONN_UNLOCK(ic);
			mbufq_drain(&mq);
			t4_free_page_pods(prsv);
			free(ddp, M_CXGBEI);
			return (ECONNRESET);
		}
		inp = sotoinpcb(ic->ic_socket);
		INP_WLOCK(inp);
		ICL_CONN_UNLOCK(ic);
		if ((inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) != 0) {
			INP_WUNLOCK(inp);
			mbufq_drain(&mq);
			t4_free_page_pods(prsv);
			free(ddp, M_CXGBEI);
			return (ECONNRESET);
		}
		mbufq_concat(&toep->ulp_pduq, &mq);
		INP_WUNLOCK(inp);

		ddp->cmp.next_buffer_offset = ctsio->kern_rel_offset +
		    first_burst;
		ddp->cmp.last_datasn = -1;
		cxgbei_insert_cmp(icc, &ddp->cmp, prsv->prsv_tag);
		*tttp = htobe32(prsv->prsv_tag);
		io_to_ddp_state(io) = ddp;
		*arg = ctsio;
		counter_u64_add(toep->ofld_rxq->rx_iscsi_ddp_setup_ok, 1);
		return (0);
	}

	/*
	 * In the middle of an I/O.  A non-NULL page pod reservation indicates
	 * that a DDP buffer is being used for the I/O.
	 */
	ddp = io_to_ddp_state(ctsio);
	if (ddp == NULL)
		goto no_ddp;
	prsv = &ddp->prsv;

	alias = (prsv->prsv_tag & pr->pr_alias_mask) >> pr->pr_alias_shift;
	alias++;
	prsv->prsv_tag &= ~pr->pr_alias_mask;
	prsv->prsv_tag |= alias << pr->pr_alias_shift & pr->pr_alias_mask;

	ddp->cmp.next_datasn = 0;
	ddp->cmp.last_datasn = -1;
	cxgbei_insert_cmp(icc, &ddp->cmp, prsv->prsv_tag);
	*tttp = htobe32(prsv->prsv_tag);
	*arg = ctsio;

	return (0);
}

void
icl_cxgbei_conn_transfer_done(struct icl_conn *ic, void *arg)
{
	struct ctl_scsiio *ctsio = arg;

	if (ctsio != NULL) {
		struct cxgbei_ddp_state *ddp;

		ddp = io_to_ddp_state(ctsio);
		MPASS(ddp != NULL);

		cxgbei_rm_cmp(ic_to_icc(ic), &ddp->cmp);
		if (ctsio->kern_data_len == ctsio->ext_data_filled ||
		    ic->ic_disconnecting) {
			t4_free_page_pods(&ddp->prsv);
			free(ddp, M_CXGBEI);
			io_to_ddp_state(ctsio) = NULL;
		}
	}
}

static void
cxgbei_limits(struct adapter *sc, void *arg)
{
	struct icl_drv_limits *idl = arg;
	struct cxgbei_data *ci;
	int max_dsl;

	if (begin_synchronized_op(sc, NULL, HOLD_LOCK, "t4lims") != 0)
		return;

	if (uld_active(sc, ULD_ISCSI)) {
		ci = sc->iscsi_ulp_softc;
		MPASS(ci != NULL);

		/*
		 * AHS is not supported by the kernel so we'll not account for
		 * it either in our PDU len -> data segment len conversions.
		 */

		max_dsl = ci->max_rx_pdu_len - ISCSI_BHS_SIZE -
		    ISCSI_HEADER_DIGEST_SIZE - ISCSI_DATA_DIGEST_SIZE;
		if (idl->idl_max_recv_data_segment_length > max_dsl)
			idl->idl_max_recv_data_segment_length = max_dsl;

		max_dsl = ci->max_tx_pdu_len - ISCSI_BHS_SIZE -
		    ISCSI_HEADER_DIGEST_SIZE - ISCSI_DATA_DIGEST_SIZE;
		if (idl->idl_max_send_data_segment_length > max_dsl)
			idl->idl_max_send_data_segment_length = max_dsl;
	}

	end_synchronized_op(sc, LOCK_HELD);
}

static int
icl_cxgbei_limits(struct icl_drv_limits *idl)
{

	/* Maximum allowed by the RFC.	cxgbei_limits will clip them. */
	idl->idl_max_recv_data_segment_length = (1 << 24) - 1;
	idl->idl_max_send_data_segment_length = (1 << 24) - 1;

	/* These are somewhat arbitrary. */
	idl->idl_max_burst_length = max_burst_length;
	idl->idl_first_burst_length = first_burst_length;

	t4_iterate(cxgbei_limits, idl);

	return (0);
}

int
icl_cxgbei_mod_load(void)
{
	int rc;

	refcount_init(&icl_cxgbei_ncons, 0);

	rc = icl_register("cxgbei", false, -100, icl_cxgbei_limits,
	    icl_cxgbei_new_conn);

	return (rc);
}

int
icl_cxgbei_mod_unload(void)
{

	if (icl_cxgbei_ncons != 0)
		return (EBUSY);

	icl_unregister("cxgbei", false);

	return (0);
}
#endif
