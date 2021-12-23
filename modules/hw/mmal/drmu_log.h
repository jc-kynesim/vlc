#ifndef _DRMU_DRMU_LOG_H
#define _DRMU_DRMU_LOG_H

#include <vlc_common.h>

#define drmu_err_log(_log, ...)       msg_Err((vlc_object_t *)(_log), __VA_ARGS__)
#define drmu_warn_log(_log, ...)      msg_Warn((vlc_object_t *)(_log), __VA_ARGS__)
#define drmu_info_log(_log, ...)      msg_Info((vlc_object_t *)(_log), __VA_ARGS__)
#define drmu_debug_log(_log, ...)     msg_Dbg((vlc_object_t *)(_log), __VA_ARGS__)

#define drmu_err(_du, ...)      drmu_err_log((_du)->log, __VA_ARGS__)
#define drmu_warn(_du, ...)     drmu_warn_log((_du)->log, __VA_ARGS__)
#define drmu_info(_du, ...)     drmu_info_log((_du)->log, __VA_ARGS__)
#define drmu_debug(_du, ...)    drmu_debug_log((_du)->log, __VA_ARGS__)

#endif

