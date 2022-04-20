### DRM ###

libdrm_vout_plugin_la_SOURCES = video_output/drm/drm_vout.c \
	video_output/drm/drmu_vlc.c video_output/drm/drmu_cma.h \
	video_output/drm/drmu_log.h video_output/drm/drmu.c \
	video_output/drm/drmu_output.h video_output/drm/drmu_output.c\
	video_output/drm/drmu_util.h video_output/drm/drmu_util.c\
	video_output/drm/drmu_xlease.c video_output/drm/drmu_atomic.c \
	video_output/drm/drmu.h \
	video_output/drm/pollqueue.c video_output/drm/pollqueue.h
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
