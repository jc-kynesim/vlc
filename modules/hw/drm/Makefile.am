drmdir = $(pluginsdir)/drm

libdrm_vout_plugin_la_SOURCES = hw/drm/drm_vout.c \
	hw/drm/drmu_vlc.c hw/drm/drmu_vlc.h hw/drm/drmu_log.h\
	hw/drm/drmu.c hw/drm/drmu_xlease.c hw/drm/drmu_atomic.c hw/drm/drmu.h hw/drm/drmu_int.h\
	hw/drm/pollqueue.c hw/drm/pollqueue.h
libdrm_vout_plugin_la_CFLAGS = $(AM_CFLAGS) -I/usr/include/libdrm
libdrm_vout_plugin_la_LIBADD = -ldrm -lxcb-randr -lxcb

drm_LTLIBRARIES = \
	libdrm_vout_plugin.la
