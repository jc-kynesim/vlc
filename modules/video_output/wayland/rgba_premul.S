#include "../../arm_neon/asm.S"

// copy_xxxa_with_premul(
//   void * dst_data,           // x0
//   int dst_stride,            // w1
//   const void * src_data,     // x2
//   int src_stride,            // w3
//   unsigned int w,            // w4
//   unsigned int h,            // w5
//   unsigned int global_alpha) // w6

// rC   R, G or B In/Out
// rA   A         In
// sG   Global alpha *257 as h scalar (v7.h[1])  In
// rT1  tmp
// rT2  tmp

.macro  mul_rgb vC, vA, sG, vT1, vT2
        umull2          \vT1\().8h, \vC\().16b, \vA\().16b
        usra            \vT1\().8h, \vT1\().8h, #8
        umull2          \vT2\().4s, \vT1\().8h, \sG
        umull           \vT1\().4s, \vT1\().4h, \sG
        shrn            \vT1\().4h, \vT1\().4s, #16
        shrn2           \vT1\().8h, \vT2\().4s, #16

        umull           \vC\().8h,  \vC\().8b,  \vA\().8b
        usra            \vC\().8h,  \vC\().8h,  #8
        umull2          \vT2\().4s, \vC\().8h,  \sG
        umull           \vC\().4s,  \vC\().4h,  \sG
        shrn            \vC\().4h,  \vC\().4s,  #16
        shrn2           \vC\().8h,  \vT2\().4s, #16

        uqrshrn         \vC\().8b,  \vC\().8h,  #8
        uqrshrn2        \vC\().16b, \vT1\().8h, #8
.endm

function copy_xxxa_with_premul_aarch64
        // Put alpha values somewhere we can use them
        mov             w7, #257
        mul             w7, w7, w6
        dup             v6.16b,  w6
        mov             v7.h[1], w7
        // *** Kludge width to mul 16
        add             w4, w4, #15
        and             w4, w4, #~15
        // Calc EOL stride add
        sub             w1, w1, w4, LSL #2
        sub             w3, w3, w4, LSL #2

2:
        mov             w6, w4

1:
        ld4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        mul_rgb         v0, v3, v7.h[1], v16, v17
        mul_rgb         v1, v3, v7.h[1], v16, v17
        mul_rgb         v2, v3, v7.h[1], v16, v17

        umull2          v19.8h, v3.16b, v6.16b           // Alpha
        usra            v19.8h, v19.8h, #8              // * 257/256
        umull           v3.8h,  v3.8b,  v6.8b
        usra            v3.8h,  v3.8h,  #8
        uqrshrn         v3.8b,  v3.8h,  #8
        uqrshrn2        v3.16b, v19.8h, #8

        st4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], #64

        subs            w6, w6, #16
        bne             1b

        add             x2, x2, w3, SXTW
        add             x0, x0, w1, SXTW
        subs            w5, w5, #1
        bne             2b

        ret

