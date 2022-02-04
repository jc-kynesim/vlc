drmdir = $(pluginsdir)/drm

libdrm_avcodec_plugin_la_SOURCES = hw/drm/drm_avcodec.c
libdrm_avcodec_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)
libdrm_avcodec_plugin_la_LIBADD = $(AVCODEC_LIBS)

if HAVE_DRM
pkglib_LTLIBRARIES += libdrm_avcodec_plugin.la
endif
