AM_CPPFLAGS                 = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES          = trebesynth.la

trebesynth_la_SOURCES      = src/trebe_synth_engine.c
trebesynth_la_LDFLAGS      = $(UNIMRCP_PLUGIN_OPTS) -lcurl

include $(top_srcdir)/build/rules/uniplugin.am