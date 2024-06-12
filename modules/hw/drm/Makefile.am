drmdir = $(pluginsdir)/drm

libdrm_avcodec_plugin_la_SOURCES = hw/drm/drm_avcodec.c
libdrm_avcodec_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)
libdrm_avcodec_plugin_la_LIBADD = $(AVCODEC_LIBS)

libdrm_gl_conv_plugin_la_SOURCES = hw/drm/drm_gl_conv.c
libdrm_gl_conv_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)

libdrm_conv_sand30_plugin_la_SOURCES = hw/drm/conv_sand30.c
libdrm_conv_sand30_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)
libdrm_conv_sand30_plugin_la_LIBADD = $(AVCODEC_LIBS)

libdrm_av_deinterlace_plugin_la_SOURCES = hw/drm/drm_av_deinterlace.c \
	codec/avcodec/drm_pic.c codec/avcodec/drm_pic.h
libdrm_av_deinterlace_plugin_la_CFLAGS = $(AM_CFLAGS) $(AVCODEC_CFLAGS)
libdrm_av_deinterlace_plugin_la_LDFLAGS = $(AM_LDFLAGS) $(AVCODEC_LDFLAGS) -lavfilter
libdrm_av_deinterlace_plugin_la_LIBADD = $(AVCODEC_LIBS)

if HAVE_DRM
drm_LTLIBRARIES = \
 libdrm_av_deinterlace_plugin.la \
 libdrm_avcodec_plugin.la \
 libdrm_conv_sand30_plugin.la \
 libdrm_gl_conv_plugin.la
endif
