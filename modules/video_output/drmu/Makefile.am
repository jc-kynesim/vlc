### DRM ###

libdrm_vout_plugin_la_SOURCES = video_output/drmu/drm_vout.c \
	video_output/drmu/drmu_vlc.c video_output/drmu/drmu_cma.h \
	video_output/drmu/drmu_log.h video_output/drmu/drmu.c \
	video_output/drmu/drmu_output.h video_output/drmu/drmu_output.c\
	video_output/drmu/drmu_util.h video_output/drmu/drmu_util.c\
	video_output/drmu/drmu_xlease.c video_output/drmu/drmu_atomic.c \
	video_output/drmu/drmu.h \
	video_output/drmu/pollqueue.c video_output/drmu/pollqueue.h
libdrm_vout_plugin_la_CFLAGS = $(AM_CFLAGS) -DHAS_VLC4=1 -pthread -I/usr/include/libdrm
libdrm_vout_plugin_la_LDFLAGS = $(AM_LDFLAGS) -pthread
libdrm_vout_plugin_la_LIBADD = -ldrm -lxcb-randr -lxcb
if HAVE_MMAL
libdrm_vout_plugin_la_CFLAGS += -DHAS_ZC_CMA=1
libdrm_vout_plugin_la_SOURCES += hw/mmal/mmal_cma_pic.h
endif
if HAVE_DRM
vout_LTLIBRARIES += libdrm_vout_plugin.la
endif
