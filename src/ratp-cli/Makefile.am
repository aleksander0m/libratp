
noinst_PROGRAMS = ratp-cli

ratp_cli_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir)/src/common \
	-I$(top_builddir)/src/common \
	-I$(top_srcdir)/src/libratp \
	-I$(top_builddir)/src/libratp \
	$(LIBEVENT_CFLAGS) \
	$(NULL)

ratp_cli_LDADD = \
	$(top_builddir)/src/common/libcommon.la \
	$(top_builddir)/src/libratp/libratp.la \
	$(NULL)

ratp_cli_LDFLAGS = \
	$(LIBEVENT_LIBS) \
	$(NULL)

ratp_cli_SOURCES = \
	ratp-cli.c \
	$(NULL)
