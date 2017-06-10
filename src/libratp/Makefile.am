
lib_LTLIBRARIES = libratp.la

libratp_la_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/src/common \
	$(LIBEVENT_CFLAGS) \
	$(NULL)

libratp_la_SOURCES = \
	ratp.h \
	ratp-base.c \
	ratp-log.h ratp-log.c \
	ratp-message.h ratp-message.c \
	ratp-link.h ratp-link.c \
	ratp-link-tty.c \
	ratp-link-fifo.c \
	ratp-sync.c \
	$(NULL)

libratp_la_LIBADD = \
	$(top_builddir)/src/common/libcommon.la \
	$(NULL)

libratp_la_LDFLAGS = \
	$(LIBEVENT_LIBS) \
	$(NULL)

include_HEADERS = \
	ratp.h \
	$(NULL)

pkgconfigdir   = $(libdir)/pkgconfig
pkgconfig_DATA = ratp.pc

EXTRA_DIST = \
	ratp.h.in \
	$(NULL)
