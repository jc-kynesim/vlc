drmdir = $(pluginsdir)/drm

libdrm_avcodec_plugin_la_SOURCES = hw/drm/drm_avcodec.c
libdrm_avcodec_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)
libdrm_avcodec_plugin_la_LIBADD = $(AVCODEC_LIBS)

libdrm_gl_conv_plugin_la_SOURCES = hw/drm/drm_gl_conv.c
libdrm_gl_conv_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)

if HAVE_DRM
pkglib_LTLIBRARIES += libdrm_avcodec_plugin.la
pkglib_LTLIBRARIES += libdrm_gl_conv_plugin.la
endif
