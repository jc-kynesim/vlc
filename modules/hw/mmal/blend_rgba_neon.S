        .syntax unified
        .arm
//      .thumb
        .text

@ [r0] xBGR dest
@ [r1] ABGR src merge
@ r2   plane alpha
@ r3   loop count

        .align  16
        .global blend_rgba_asm_neon
#ifdef __ELF__
        .type   blend_rgba_asm_neon, %function
#endif

blend_rgba_asm_neon:

        vpush   {q4}

        movw     r12, #257
        mul      r2,  r12
        vmov     s16, r2
        vmov.i8  q11, #0xff
        vmov.u8  q10, #1
        vmov.u32 q12, #0xff000000
1:
        vld1.32 {q0}, [r1]!
        @ Alpha in hi byte of each word
        vshr.u32  q8, q0, #24
        vmul.u32  q8, q8, d8[0]
        vrshr.u32 q8, q8, #16  @ 4 mix alpha values
        vmul.u32  q8, q10      @ dup every alpha into all 4 bytes of its word
        vsub.u8   q9, q11, q8  @ gen the cplmnt

        vld1.32 {q2}, [r0]

        vmull.u8  q1, d1, d17
        vmull.u8  q0, d0, d16
        vmlal.u8  q1, d5, d19
        vmlal.u8  q0, d4, d18

        vsra.u16  q1, q1, #8
        vsra.u16  q0, q0, #8

        subs      r3, #1
        vrshrn.u16 d0, q0, #8
        vrshrn.u16 d1, q1, #8

        vbit      q0, q12, q12 @ Set alpha to #255

        vst1.32  {q0}, [r0]!
        bne       1b

        vpop     {q4}
        bx        lr



