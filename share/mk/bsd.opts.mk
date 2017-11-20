# $FreeBSD$
#
# Option file for src builds.
#
# Users define WITH_FOO and WITHOUT_FOO on the command line or in /etc/src.conf
# and /etc/make.conf files. These translate in the build system to MK_FOO={yes,no}
# with (usually) sensible defaults.
#
# Makefiles must include bsd.opts.mk after defining specific MK_FOO options that
# are applicable for that Makefile (typically there are none, but sometimes there
# are exceptions). Recursive makes usually add MK_FOO=no for options that they wish
# to omit from that make.
#
# Makefiles must include bsd.mkopt.mk before they test the value of any MK_FOO
# variable.
#
# Makefiles may also assume that this file is included by bsd.own.mk should it
# need variables defined there prior to the end of the Makefile where
# bsd.{subdir,lib.bin}.mk is traditionally included.
#
# The old-style YES_FOO and NO_FOO are being phased out. No new instances of them
# should be added. Old instances should be removed since they were just to
# bridge the gap between FreeBSD 4 and FreeBSD 5.
#
# Makefiles should never test WITH_FOO or WITHOUT_FOO directly (although an
# exception is made for _WITHOUT_SRCONF which turns off this mechanism
# completely).
#

.if !target(__<bsd.opts.mk>__)
__<bsd.opts.mk>__:

.if !defined(_WITHOUT_SRCCONF)
#
# Define MK_* variables (which are either "yes" or "no") for users
# to set via WITH_*/WITHOUT_* in /etc/src.conf and override in the
# make(1) environment.
# These should be tested with `== "no"' or `!= "no"' in makefiles.
# The NO_* variables should only be set by makefiles for variables
# that haven't been converted over.
#

# Only these options are used by bsd.*.mk. KERBEROS and OPENSSH are
# unfortunately needed to support statically linking the entire
# tree. su(1) wouldn't link since it depends on PAM which depends on
# ssh libraries when building with OPENSSH, and likewise for KERBEROS.

# All other variables used to build /usr/src live in src.opts.mk
# and variables from both files are documented in src.conf(5).

__DEFAULT_YES_OPTIONS = \
    ASSERT_DEBUG \
    DEBUG_FILES \
    DOCCOMPRESS \
    INCLUDES \
    INSTALLLIB \
    KERBEROS \
    MAKE_CHECK_USE_SANDBOX \
    MAN \
    MANCOMPRESS \
    NIS \
    NLS \
    OPENSSH \
    PIE \
    PROFILE \
    SSP \
    SYMVER \
    TESTS \
    TOOLCHAIN \
    WARNS

__DEFAULT_YES_OPTIONS+= \
    CHERI_SHARED

__DEFAULT_NO_OPTIONS = \
    CCACHE_BUILD \
    CTF \
    INSTALL_AS_USER \
    STALE_STAGED

__DEFAULT_NO_OPTIONS+= \
    CHERI_EXACT_EQUALS \
    CHERI_PURE \
    CHERI_SHARED_PROG \
    CHERI128 \
    CHERI256 \
    CHERIBSDBOX \
    DEMO_VULNERABILITIES

__DEFAULT_DEPENDENT_OPTIONS = \
    MAKE_CHECK_USE_SANDBOX/TESTS \
    STAGING_MAN/STAGING \
    STAGING_PROG/STAGING \
    STALE_STAGED/STAGING \

.if defined(WITH_CHERI)
.warning WITH_CHERI should not be set directly.
.warning Use WITH_CHERI128 or WITH_CHERI256 instead.
.if !defined(WITH_CHERI128) && !defined(CHERI256)
WITH_CHERI256:=	yes
.endif
.endif

.include <bsd.mkopt.mk>

.if ${MK_CHERI128} == "yes" && ${MK_CHERI256} == "yes"
.error WITH_CHERI128 and WITH_CHERI256 are incompatible.
.endif

.if ${MK_CHERI128} == "yes" || ${MK_CHERI256} == "yes"
MK_CHERI:=	yes
MK_CLANG:=	no
# We want to use libc++ for CHERI (even when targeting MIPS)
MK_GNUCXX:=	no
MK_LIBCPLUSPLUS:=yes
# LLVM libunwind is needed for libc++
MK_LLVM_LIBUNWIND:=	yes
# GROFF is broken when building WITH_CHERI_PURE and it will be removed
# soon anyway
MK_GROFF:=	no
# Build cheribsdbox by default so that we have a emergency MIPS tool if the
# CHERI world is broken
MK_CHERIBSDBOX:=	yes
.else
MK_CHERI:=	no
.endif

.if ${MK_CHERI_SHARED} == no
MK_CHERI_SHARED_PROG:=	no
.endif
.if ${MK_CHERI_SHARED_PROG} == no
MK_CASPER:=	no
.endif

#
# Supported NO_* options (if defined, MK_* will be forced to "no",
# regardless of user's setting).
#
# These are transitional and will disappaer in the FreeBSD 12.
#
.for var in \
    CTF \
    DEBUG_FILES \
    INSTALLLIB \
    MAN \
    PROFILE \
    WARNS
.if defined(NO_${var})
.warning "NO_${var} is defined, but deprecated. Please use MK_${var}=no instead."
MK_${var}:=no
.endif
.endfor

.include <bsd.cpu.mk>

.endif # !_WITHOUT_SRCCONF

.endif
