#include "../../arm_neon/asm.S"
        .align 16
        .arch armv7-a
        .syntax unified
#if HAVE_AS_FPU_DIRECTIVE
        .fpu neon-vfpv4
#endif

// Copy pix

.macro  piccpy_to_8, bit_depth
        subs     r2, #128
        vpush    {q4-q7}
        blt      2f
1:
        vldm     r1!, {q0-q7}
        subs     r2, #128
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vqrshrn.u16 d1,  q1,  #\bit_depth - 8
        vqrshrn.u16 d2,  q2,  #\bit_depth - 8
        vqrshrn.u16 d3,  q3,  #\bit_depth - 8
        vldm     r1!, {q8-q15}
        vqrshrn.u16 d4,  q4,  #\bit_depth - 8
        vqrshrn.u16 d5,  q5,  #\bit_depth - 8
        vqrshrn.u16 d6,  q6,  #\bit_depth - 8
        vqrshrn.u16 d7,  q7,  #\bit_depth - 8
        vqrshrn.u16 d8,  q8,  #\bit_depth - 8
        vqrshrn.u16 d9,  q9,  #\bit_depth - 8
        vqrshrn.u16 d10, q10, #\bit_depth - 8
        vqrshrn.u16 d11, q11, #\bit_depth - 8
        vqrshrn.u16 d12, q12, #\bit_depth - 8
        vqrshrn.u16 d13, q13, #\bit_depth - 8
        vqrshrn.u16 d14, q14, #\bit_depth - 8
        vqrshrn.u16 d15, q15, #\bit_depth - 8
        vstm     r0!, {q0-q7}
        bge      1b
2:
        adds     r2, #64
        blt      1f

        vldm     r1!, {q0-q7}
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vqrshrn.u16 d1,  q1,  #\bit_depth - 8
        vqrshrn.u16 d2,  q2,  #\bit_depth - 8
        vqrshrn.u16 d3,  q3,  #\bit_depth - 8
        vqrshrn.u16 d4,  q4,  #\bit_depth - 8
        vqrshrn.u16 d5,  q5,  #\bit_depth - 8
        vqrshrn.u16 d6,  q6,  #\bit_depth - 8
        vqrshrn.u16 d7,  q7,  #\bit_depth - 8
        vstm     r0!, {q0-q3}
1:
        adds     r2, #32
        blt      1f

        vldm     r1!, {q0-q3}
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vqrshrn.u16 d1,  q1,  #\bit_depth - 8
        vqrshrn.u16 d2,  q2,  #\bit_depth - 8
        vqrshrn.u16 d3,  q3,  #\bit_depth - 8
        vstm     r0!, {q0-q1}
1:
        adds     r2, #16
        blt      1f

        vldm     r1!, {q0-q1}
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vqrshrn.u16 d1,  q1,  #\bit_depth - 8
        vstm     r0!, {q0}
1:
        adds     r2, #8
        blt      1f

        vldm     r1!, {q0}
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vstr     d0, [r0]
        add      r0, #8
1:
        adds     r2, #4
        blt      1f

        vldr     d0, [r1]
        vqrshrn.u16 d0,  q0,  #\bit_depth - 8
        vstr     s0, [r0]
1:
        vpop     {q4-q7}
        bx       lr
.endm


@ [r0] Dest
@ [r1] Src
@  r2  Pels
function mmal_piccpy_10_to_8_neon
        piccpy_to_8 10

