#ifndef RPI_PROFILE_H
#define RPI_PROFILE_H

#include <stdint.h>
#include <inttypes.h>

#ifndef RPI_PROFILE
#define RPI_PROFILE 0
#endif

#if RPI_PROFILE

#include "v7_pmu.h"

#ifdef RPI_PROC_ALLOC
#define X volatile
#define Z =0
#else
#define X extern volatile
#define Z
#endif

X uint64_t av_rpi_prof0_cycles Z;
X unsigned int av_rpi_prof0_cnt Z;
#define RPI_prof0_MAX_DURATION 100000

X uint64_t av_rpi_prof1_cycles Z;
X unsigned int av_rpi_prof1_cnt Z;
#define RPI_prof1_MAX_DURATION 100000

X uint64_t av_rpi_prof2_cycles Z;
X unsigned int av_rpi_prof2_cnt Z;
#define RPI_prof2_MAX_DURATION 10000

X uint64_t av_rpi_prof_n_cycles[128];
X unsigned int av_rpi_prof_n_cnt[128];
#define RPI_prof_n_MAX_DURATION 10000


#undef X
#undef Z

#define PROFILE_INIT()\
do {\
    enable_pmu();\
    enable_ccnt();\
} while (0)

#define PROFILE_START()\
do {\
    volatile uint32_t perf_1 = read_ccnt();\
    volatile uint32_t perf_2


#define PROFILE_ACC(x)\
    perf_2 = read_ccnt();\
    {\
        const uint32_t duration = perf_2 - perf_1;\
        if (duration < RPI_##x##_MAX_DURATION)\
        {\
            av_rpi_##x##_cycles += duration;\
            av_rpi_##x##_cnt += 1;\
        }\
    }\
} while(0)


#define PROFILE_ACC_N(n)\
    if ((n) >= 0) {\
        perf_2 = read_ccnt();\
        {\
            const uint32_t duration = perf_2 - perf_1;\
            if (duration < RPI_prof_n_MAX_DURATION)\
            {\
                av_rpi_prof_n_cycles[n] += duration;\
                av_rpi_prof_n_cnt[n] += 1;\
            }\
        }\
    }\
} while(0)

#define PROFILE_PRINTF(x)\
    printf("%-20s cycles=%14" PRIu64 ";  cnt=%8u;  avg=%5" PRIu64 "\n", #x, av_rpi_##x##_cycles, av_rpi_##x##_cnt,\
        av_rpi_##x##_cnt == 0 ? (uint64_t)0 : av_rpi_##x##_cycles / (uint64_t)av_rpi_##x##_cnt)

#define PROFILE_PRINTF_N(n)\
    printf("prof[%d] cycles=%14" PRIu64 ";  cnt=%8u;  avg=%5" PRIu64 "\n", (n), av_rpi_prof_n_cycles[n], av_rpi_prof_n_cnt[n],\
        av_rpi_prof_n_cnt[n] == 0 ? (uint64_t)0 : av_rpi_prof_n_cycles[n] / (uint64_t)av_rpi_prof_n_cnt[n])

#define PROFILE_CLEAR_N(n) \
do {\
    av_rpi_prof_n_cycles[n] = 0;\
    av_rpi_prof_n_cnt[n] = 0;\
} while(0)

#else

// No profile
#define PROFILE_INIT()
#define PROFILE_START()
#define PROFILE_ACC(x)
#define PROFILE_ACC_N(x)
#define PROFILE_PRINTF(x)
#define PROFILE_PRINTF_N(x)
#define PROFILE_CLEAR_N(n)

#endif

#endif

