AM_CPPFLAGS                 = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES          = treberecog.la

treberecog_la_SOURCES      = src/trebe_recog_engine.c
treberecog_la_LDFLAGS      = $(UNIMRCP_PLUGIN_OPTS) -lwebsockets -ljson-c

include $(top_srcdir)/build/rules/uniplugin.am