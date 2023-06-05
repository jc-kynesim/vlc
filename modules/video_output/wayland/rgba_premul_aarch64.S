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
//
// vC = (vC * vA * 257/256 * sG + 0x800000) >> 24
.macro  mul_rgb vC, vA, sG, vT1, vT2

        umull           \vT2\().8h, \vC\().8b,  \vA\().8b
        umull2          \vT1\().8h, \vC\().16b, \vA\().16b

        usra            \vT2\().8h, \vT2\().8h, #8
        usra            \vT1\().8h, \vT1\().8h, #8

        umull           \vC\().4s,  \vT2\().4h, \sG
        umull2          \vT2\().4s, \vT2\().8h, \sG
	uzp2		\vC\().8h,  \vC\().8h,  \vT2\().8h

        umull           \vT2\().4s, \vT1\().4h, \sG
        umull2          \vT1\().4s, \vT1\().8h, \sG
	uzp2		\vT2\().8h, \vT2\().8h, \vT1\().8h

        uqrshrn         \vC\().8b,  \vC\().8h,  #8
        uqrshrn2        \vC\().16b, \vT2\().8h, #8
.endm

// rA   A         In/Out
// vG   Global alpha duped bytewise into vector  In
// rT1  tmp
//
// vA = (vA * vG *257/256 + 0x80) >> 8
.macro  mul_a vA, vG, vT1, vT2
        umull           \vT1\().8h, \vA\().8b,  \vG\().8b           // Alpha
        umull2          \vT2\().8h, \vA\().16b, \vG\().16b
        usra            \vT1\().8h, \vT1\().8h, #8              // * 257/256
        usra            \vT2\().8h, \vT2\().8h, #8
        uqrshrn         \vA\().8b,  \vT1\().8h, #8
        uqrshrn2        \vA\().16b, \vT2\().8h, #8
.endm

function copy_xxxa_with_premul_aarch64
        mov             x15, lr         // Stash return addr
        // Sanity check w & h
        cbz             w4, 90f
        cbz             w5, 90f

        // Put alpha values somewhere we can use them
        mov             w7, #257        // Would like 258 but 258*255 overflows h
        mul             w7, w7, w6
        dup             v6.16b,  w6
        mov             v7.h[1], w7

        // Calc EOL stride add
        sub             w1, w1, w4, LSL #2
        sub             w3, w3, w4, LSL #2

        // Deal with very short stuff separately
        // Saves annoying conditionals later
        cmp             w4, #16
        bge             22f

        // Loop for w < 16
        mov             w6, w4
1:
        bl              50f
        add             x2, x2, w3, SXTW
        add             x0, x0, w1, SXTW
        subs            w5, w5, #1
        bne             1b
        b               90f

        // Top of height loop
20:
        add             x2, x2, w3, SXTW
        add             x0, x0, w1, SXTW
        subs            w5, w5, #1
        beq             90f

22:
        // Align destination before main loop
        tst             x0, #63
        mov             w6, w4
        beq             1f

        mov             w7, #16
        ubfm            w6, w0, #2, #5
        sub             w6, w7, w6
        bl              50f
        sub             w6, w4, w6
1:
        // If w % 16 != 0 then -16 so the main loop runs 1 fewer times with
        // the remainder done in the tail
        tst             w6, #15
        bne             15f

        // Top of width loop
10:
        ld4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64

        mul_rgb         v0, v3, v7.h[1], v16, v17
        mul_rgb         v1, v3, v7.h[1], v16, v17
        mul_rgb         v2, v3, v7.h[1], v16, v17
        mul_a           v3, v6, v16, v17

        st4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], #64

15:
        subs            w6, w6, #16
        bgt             10b
        beq             20b             // No tail

        // Tail
        bl              50f
        b               20b

        // Return
90:
        mov             lr, x15
        ret

// Tail & Head core
//
// w6   Noof pixels to convert
//      Only bottom 3 bits considered, left unchanged on exit
50:
        tbz             w6, #3, 1f
        ld4             {v0.8b, v1.8b, v2.8b, v3.8b}, [x2], #32
1:      tbz             w6, #2, 1f
        ld4             {v0.b, v1.b, v2.b, v3.b}[8],  [x2], #4
        ld4             {v0.b, v1.b, v2.b, v3.b}[9],  [x2], #4
        ld4             {v0.b, v1.b, v2.b, v3.b}[10], [x2], #4
        ld4             {v0.b, v1.b, v2.b, v3.b}[11], [x2], #4
1:      tbz             w6, #1, 1f
        ld4             {v0.b, v1.b, v2.b, v3.b}[12], [x2], #4
        ld4             {v0.b, v1.b, v2.b, v3.b}[13], [x2], #4
1:      tbz             w6, #0, 1f
        ld4             {v0.b, v1.b, v2.b, v3.b}[14], [x2], #4
1:

        mul_rgb         v0, v3, v7.h[1], v16, v17
        mul_rgb         v1, v3, v7.h[1], v16, v17
        mul_rgb         v2, v3, v7.h[1], v16, v17
        mul_a           v3, v6, v16, v17

        tbz             w6, #3, 1f
        st4             {v0.8b, v1.8b, v2.8b, v3.8b}, [x0], #32
1:      tbz             w6, #2, 1f
        st4             {v0.b, v1.b, v2.b, v3.b}[8],  [x0], #4
        st4             {v0.b, v1.b, v2.b, v3.b}[9],  [x0], #4
        st4             {v0.b, v1.b, v2.b, v3.b}[10], [x0], #4
        st4             {v0.b, v1.b, v2.b, v3.b}[11], [x0], #4
1:      tbz             w6, #1, 1f
        st4             {v0.b, v1.b, v2.b, v3.b}[12], [x0], #4
        st4             {v0.b, v1.b, v2.b, v3.b}[13], [x0], #4
1:      tbz             w6, #0, 1f
        st4             {v0.b, v1.b, v2.b, v3.b}[14], [x0], #4
1:
        ret

