AC_INIT([spindle], m4_esyscmd([tr -d '\n' < ../../VERSION]), m4_esyscmd([tr -d '\n' < ../../BUG_EMAIL]))
AC_CONFIG_MACRO_DIR([../../m4])
AC_CONFIG_AUX_DIR([../../scripts])
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])
AC_CONFIG_HEADERS([config.h])
AM_PROG_AR
AC_PROG_CC
AM_PROG_CC_C_O
AC_CONFIG_FILES([logging/Makefile biter/Makefile shm_cache/Makefile client_comlib/Makefile client/Makefile auditclient/Makefile subaudit/Makefile beboot/Makefile spindle_api/Makefile ldsolookup/Makefile Makefile])
AC_PROG_LN_S
LT_INIT
SPINDLE_BUILD_ROOT=`pwd`/../..
SPINDLE_SOURCE_ROOT=`realpath ${srcdir}/../..`

#Include common macro library and call macros needed by the client tier
m4_include([../../m4/lx_detect_bluegene.m4])
m4_include([../../configure.common.ac])

SPINDLE_NETWORK_CONFIG
SPINDLE_OS_DETECT
SPINDLE_RESOURCE_MANAGER
SPINDLE_COMM_MODE
SPINDLE_SEC_ARGS
SPINDLE_OPTIONAL_FEATURES
SPINDLE_FLUX_DETECT
SPINDLE_GLIBC_BUGS
SPINDLE_LDSO_PATCH
SPINDLE_GLIBC_LOCATION
SPINDLE_STATIC_LINK
CHECK_MMAP([cl])

AC_SUBST([LIBSPINDLE_LIB_VERSION],[`$srcdir/../../LIB_VERSION libspindle`])
AC_OUTPUT

