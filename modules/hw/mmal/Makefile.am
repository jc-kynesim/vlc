include $(top_srcdir)/modules/common.am
mmaldir = $(pluginsdir)/mmal

AM_CFLAGS += -pthread $(CFLAGS_mmal)
AM_LDFLAGS += -pthread -rpath '$(mmaldir)' $(LDFLAGS_mmal)

mmal_LTLIBRARIES =

if HAVE_MMAL
libmmal_vout_plugin_la_SOURCES = vout.c mmal_cma.c mmal_picture.c subpic.c\
  mmal_cma.h mmal_cma_int.h mmal_picture.h subpic.h transform_ops.h\
  mmal_piccpy_neon.S
libmmal_vout_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_vout_plugin_la_LDFLAGS = $(AM_LDFLAGS) -lm -lX11 -lXrandr
libmmal_vout_plugin_la_LIBADD = $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_vout_plugin.la

libmmal_codec_plugin_la_SOURCES = codec.c mmal_cma.c mmal_picture.c subpic.c\
  mmal_cma.h mmal_cma_int.h mmal_picture.h subpic.h transform_ops.h\
  blend_rgba_neon.S mmal_piccpy_neon.S
libmmal_codec_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_codec_plugin_la_LDFLAGS = $(AM_LDFLAGS)
libmmal_codec_plugin_la_LIBADD = $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_codec_plugin.la

libmmal_deinterlace_plugin_la_SOURCES = deinterlace.c mmal_picture.c mmal_cma.c\
  mmal_cma.h mmal_cma_int.h mmal_picture.h transform_ops.h\
  mmal_piccpy_neon.S
libmmal_deinterlace_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_deinterlace_plugin_la_LDFLAGS = $(AM_LDFLAGS)
libmmal_deinterlace_plugin_la_LIBADD = $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_deinterlace_plugin.la

libmmal_converter_plugin_la_SOURCES = converter_mmal.c mmal_cma.c mmal_picture.c\
  mmal_cma.h mmal_cma_int.h mmal_picture.h transform_ops.h\
  mmal_piccpy_neon.S
libmmal_converter_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_converter_plugin_la_LDFLAGS = $(AM_LDFLAGS)
libmmal_converter_plugin_la_LIBADD = $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_converter_plugin.la
endif

libmmal_xsplitter_plugin_la_SOURCES = xsplitter.c
libmmal_xsplitter_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_xsplitter_plugin_la_LDFLAGS = $(AM_LDFLAGS)
libmmal_xsplitter_plugin_la_LIBADD = $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_xsplitter_plugin.la


if HAVE_MMAL_AVCODEC
libmmal_avcodec_plugin_la_SOURCES = mmal_avcodec.c mmal_cma_drmprime.c mmal_cma.c\
  mmal_picture.c\
  mmal_cma.h mmal_cma_int.h mmal_cma_drmprime.h mmal_picture.h transform_ops.h\
  mmal_piccpy_neon.S
libmmal_avcodec_plugin_la_CFLAGS = $(AM_CFLAGS)
libmmal_avcodec_plugin_la_LDFLAGS = $(AM_LDFLAGS)
libmmal_avcodec_plugin_la_LIBADD = $(AVFORMAT_LIBS) $(AVUTIL_LIBS) $(LIBS_mmal)
mmal_LTLIBRARIES += libmmal_avcodec_plugin.la
endif

