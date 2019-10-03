dnl Process this file with autoconf to produce a configure script.
AC_PREREQ([2.68])

dnl The libratp version number
m4_define([ratp_major_version], [0])
m4_define([ratp_minor_version], [0])
m4_define([ratp_micro_version], [7])
m4_define([ratp_version],
          [ratp_major_version.ratp_minor_version.ratp_micro_version])

AC_INIT([libratp], [ratp_version], [aleksander@aleksander.es])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 foreign no-define no-dist-gzip dist-xz tar-ustar -Wno-portability])
AM_MAINTAINER_MODE([enable])

dnl Support silent build rules. Disable
dnl by either passing --disable-silent-rules to configure or passing V=1
dnl to make
AM_SILENT_RULES([yes])

dnl Required programs
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL
AX_PTHREAD

dnl Initialize libtool
LT_PREREQ([2.2])
LT_INIT

dnl Specific warnings to always use
LIBRATP_COMPILER_WARNINGS

dnl Version stuff
RATP_MAJOR_VERSION=ratp_major_version
RATP_MINOR_VERSION=ratp_minor_version
RATP_MICRO_VERSION=ratp_micro_version
RATP_VERSION=ratp_version
AC_SUBST(RATP_MAJOR_VERSION)
AC_SUBST(RATP_MINOR_VERSION)
AC_SUBST(RATP_MICRO_VERSION)
AC_SUBST(RATP_VERSION)

dnl Dependencies
LIBEVENT_VERSION=2

dnl libevent (mandatory)
PKG_CHECK_MODULES([LIBEVENT], [libevent >= $LIBEVENT_VERSION libevent_pthreads])

dnl Documentation
GTK_DOC_CHECK(1.0)

AC_CONFIG_FILES([Makefile
                 src/Makefile
                 src/common/Makefile
                 src/libratp/Makefile
                 src/libratp/ratp.h
                 src/libratp/ratp.pc
                 doc/Makefile
                 doc/reference/Makefile
                 doc/reference/version.xml])
AC_OUTPUT

echo "
    libratp $VERSION
    ==============================================

    compiler:         ${CC}
    cflags:           ${CFLAGS}
    ldflags:          ${LDFLAGS}
    maintainer mode:  ${USE_MAINTAINER_MODE}
    documentation:    ${enable_gtk_doc}
"
