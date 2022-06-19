### DRM PRIME ###
drmprimedir = $(pluginsdir)/drmprime

libdrmprime_deinterlace_plugin_la_SOURCES = hw/drmprime/deinterlace.c
libdrmprime_deinterlace_plugin_la_CFLAGS = $(AM_CFLAGS)
libdrmprime_deinterlace_plugin_la_LDFLAGS = $(AM_LDFLAGS)

if HAVE_DRM
drmprime_LTLIBRARIES = libdrmprime_deinterlace_plugin.la
endif
