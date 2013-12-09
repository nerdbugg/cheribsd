/*-
 * Copyright (c) 2012-2013 Robert N. M. Watson
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
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

#if __has_feature(capabilities)
#define	USE_C_CAPS
#endif

#include <sys/types.h>

#include <machine/cheri.h>

#include <md5.h>
#include <stdlib.h>

#include "cmemcpy.h"
#include "cheritest-helper.h"

#ifdef USE_C_CAPS
int	invoke(register_t op, size_t len, __capability char *data_input,
	    __capability char *data_output);
#else
int	invoke(register_t op, size_t len);
#endif

static int
#ifdef USE_C_CAPS
invoke_md5(size_t len, __capability char *data_input,
  __capability char *data_output)
#else
invoke_md5(size_t len)
#endif
{
	MD5_CTX md5context;
	char buf[33], ch;
	u_int count;

	MD5Init(&md5context);
	for (count = 0; count < len; count++) {
#ifdef USE_C_CAPS
		/* XXXRW: Want a CMD5Update() to avoid copying byte by byte. */
		ch = data_input[count];
#else
		memcpy_fromcap(&ch, 3, count, sizeof(ch));
#endif
		MD5Update(&md5context, &ch, sizeof(ch));
	}
	MD5End(&md5context, buf);
#ifdef USE_C_CAPS
	for (count = 0; count < sizeof(buf); count++)
		data_output[count] = buf[count];
#else
	memcpy_tocap(4, buf, 0, sizeof(buf));
#endif

	/*
	 * Invoke getpid() to trigger kernel protection features.  Should
	 * mostly be a nop.
	 */
	__asm__ __volatile__ ("syscall 20");

	return (123456);
}

/*
 * Sample sandboxed code.  Calculate an MD5 checksum of the data arriving via
 * c3, and place the checksum in c4.  a0 will hold input data length.  a1
 * indicates whether we should try a system call (abort()).  c4 must be (at
 * least) 33 bytes.
 */
int
#ifdef USE_C_CAPS
invoke(register_t op, size_t len, __capability char *data_input,
    __capability char *data_output)
#else
invoke(register_t op, size_t len)
#endif
{
	int ret;

	switch (op) {
	case CHERITEST_HELPER_OP_MD5:
#ifdef USE_C_CAPS
		return (invoke_md5(len, data_input, data_output));
#else
		return (invoke_md5(len));
#endif

	case CHERITEST_HELPER_OP_ABORT:
		abort();

	case CHERITEST_HELPER_OP_SPIN:
		while (1);
	}
	return (ret);
}
