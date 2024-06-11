#include "../../arm_neon/asm.S"
        .align 16
        .arch armv7-a
        .syntax unified
#if HAVE_AS_FPU_DIRECTIVE
        .fpu neon-vfpv4
#endif

@ blend_rgbx_rgba_neon

@ Implements /255 as ((x * 257) + 0x8000) >> 16
@ This generates something in the range [(x+126)/255, (x+127)/255] which is good enough

@ There is advantage to aligning src and/or dest - dest gives a bit more due to being used twice



@ [r0] RGBx dest      loaded into d20-d23
@ [r1] RGBA src merge loaded into d16-d19
@ r2   plane alpha
@ r3   count (pixels)

.macro blend_main sR, sG, sB, sA, dR, dG, dB, dA

        push      { r4, lr }

        vdup.u8    d7,  r2

        subs       r3,  #8
        vmov.u8    d6,  #0xff

        blt        2f

        @ If < 16 bytes to move then don't bother trying to align
        @ (a) This means the the align doesn't need to worry about r3 underflow
        @ (b) The overhead would be greater than any gain
        cmp        r3,  #8
        mov        r4,  r3
        ble        1f

        @ Align r1 on a 32 byte boundary
        neg        r3,  r0
        ubfx       r3,  r3,  #2,  #3

        cmp        r3,  #0
        blne       10f

        sub        r3,  r4,  r3

1:
        vld4.8    {d16, d17, d18, d19}, [r1]

1:
        vmull.u8   q15, \sA, d7

        vld4.8    {d20, d21, d22, d23}, [r0]

        vsra.u16   q15, q15, #8
        subs       r3,  #8
        vrshrn.u16 d31, q15, #8
        vsub.u8    d30, d6,  d31

        vmull.u8   q12, \sR, d31
        vmull.u8   q13, \sG, d31
        vmull.u8   q14, \sB, d31
        addge      r1,  #32

        vmlal.u8   q12, \dR, d30
        vmlal.u8   q13, \dG, d30
        vmlal.u8   q14, \dB, d30
        vld4.8    {d16, d17, d18, d19}, [r1]

        vsra.u16   q12, q12, #8         @ * 257/256
        vsra.u16   q13, q13, #8
        vsra.u16   q14, q14, #8

        vrshrn.u16 \dR, q12, #8
        vrshrn.u16 \dG, q13, #8
        vrshrn.u16 \dB, q14, #8
        vmov.u8    \dA, #0xff

        vst4.8    {d20, d21, d22, d23}, [r0]!
        bge        1b
        add        r1,  #32

2:
        cmp        r3,  #-8
        blgt       10f

        pop       { r4, pc }


// Partial version
// Align @ start & deal with tail
10:
        lsls       r2,  r3,  #30        @ b2 -> C, b1 -> N
        mov        r2,  r0
        bcc        1f
        vld4.8    {d16[0], d17[0], d18[0], d19[0]}, [r1]!
        vld4.8    {d20[0], d21[0], d22[0], d23[0]}, [r2]!
        vld4.8    {d16[1], d17[1], d18[1], d19[1]}, [r1]!
        vld4.8    {d20[1], d21[1], d22[1], d23[1]}, [r2]!
        vld4.8    {d16[2], d17[2], d18[2], d19[2]}, [r1]!
        vld4.8    {d20[2], d21[2], d22[2], d23[2]}, [r2]!
        vld4.8    {d16[3], d17[3], d18[3], d19[3]}, [r1]!
        vld4.8    {d20[3], d21[3], d22[3], d23[3]}, [r2]!
1:
        bpl        1f
        vld4.8    {d16[4], d17[4], d18[4], d19[4]}, [r1]!
        vld4.8    {d20[4], d21[4], d22[4], d23[4]}, [r2]!
        vld4.8    {d16[5], d17[5], d18[5], d19[5]}, [r1]!
        vld4.8    {d20[5], d21[5], d22[5], d23[5]}, [r2]!
1:
        tst        r3,  #1
        beq        1f
        vld4.8    {d16[6], d17[6], d18[6], d19[6]}, [r1]!
        vld4.8    {d20[6], d21[6], d22[6], d23[6]}, [r2]!
1:
        @ Set conditions for later
        lsls       r2,  r3,  #30        @ b2 -> C, b1 -> N

        vmull.u8   q15, \sA, d7
        vsra.u16   q15, q15, #8
        vrshrn.u16 d31, q15, #8
        vsub.u8    d30, d6,  d31

        vmull.u8   q12, \sR, d31
        vmull.u8   q13, \sG, d31
        vmull.u8   q14, \sB, d31

        vmlal.u8   q12, \dR, d30
        vmlal.u8   q13, \dG, d30
        vmlal.u8   q14, \dB, d30

        vsra.u16   q12, q12, #8
        vsra.u16   q13, q13, #8
        vsra.u16   q14, q14, #8

        vrshrn.u16 \dR, q12, #8
        vrshrn.u16 \dG, q13, #8
        vrshrn.u16 \dB, q14, #8
        vmov.u8    \dA, #0xff

        bcc        1f
        vst4.8    {d20[0], d21[0], d22[0], d23[0]}, [r0]!
        vst4.8    {d20[1], d21[1], d22[1], d23[1]}, [r0]!
        vst4.8    {d20[2], d21[2], d22[2], d23[2]}, [r0]!
        vst4.8    {d20[3], d21[3], d22[3], d23[3]}, [r0]!
1:
        bpl        1f
        vst4.8    {d20[4], d21[4], d22[4], d23[4]}, [r0]!
        vst4.8    {d20[5], d21[5], d22[5], d23[5]}, [r0]!
1:
        tst        r3,  #1
        bxeq       lr
        vst4.8    {d20[6], d21[6], d22[6], d23[6]}, [r0]!

        bx         lr

.endm


@ [r0] RGBx dest      (Byte order: R, G, B, x)
@ [r1] RGBA src merge (Byte order: R, G, B, A)
@ r2   plane alpha
@ r3   count (pixels)

@ Whilst specified as RGBx+RGBA the only important part is the position of
@ alpha, the other components are all treated the same

@ [r0] RGBx dest      (Byte order: R, G, B, x)
@ [r1] RGBA src merge (Byte order: R, G, B, A) - same as above
@ r2   plane alpha
@ r3   count (pixels)
        .align  16
function blend_rgbx_rgba_neon
        blend_main d16, d17, d18, d19, d20, d21, d22, d23


@ [r0] RGBx dest      (Byte order: R, G, B, x)
@ [r1] RGBA src merge (Byte order: B, G, R, A) - B / R swapped
@ r2   plane alpha
@ r3   count (pixels)
        .align  16
function blend_bgrx_rgba_neon
        blend_main d18, d17, d16, d19, d20, d21, d22, d23



