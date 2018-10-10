        .syntax unified
        .arm
//      .thumb
        .text

@ [r0] xBGR dest
@ [r1] ABGR src merge
@ r2   plane alpha
@ r3   pels

        .align  16
        .global blend_rgbx_rgba_neon
#ifdef __ELF__
        .type   blend_rgbx_rgba_neon, %function
#endif

blend_rgbx_rgba_neon:

        vpush     { q4 }                @ With reworked reg alloc this could be avoided
        movw       r12, #257
        vmov.i8    q11, #0xff
        mul        r2,  r12
        vmov.u8    q10, #1
        subs       r3,  #8
        vmov       s16, r2
        vmov.u32   q12, #0xff000000
        blt        2f

1:
        vld1.32   { q0,q1 }, [r1]!
        @ Alpha in hi byte of each word
        vshr.u32   q9,  q1,  #24
        vshr.u32   q8,  q0,  #24
        vmul.u32   q9,  q9,  d8[0]
        vmul.u32   q8,  q8,  d8[0]
        vrshr.u32  q9,  q9,  #16
        vrshr.u32  q8,  q8,  #16        @ 4 mix alpha values
        vmul.u32   q9,  q10
        vmul.u32   q8,  q10             @ dup every alpha into all 4 bytes of its word

        vld1.32   {q14,q15}, [r0]

        vmull.u8   q3,  d3,  d19
        vmull.u8   q2,  d2,  d18
        vsub.u8    q9,  q11, q9         @ gen the cplmnt

        vmull.u8   q1,  d1,  d17
        vmull.u8   q0,  d0,  d16
        vsub.u8    q8,  q11, q8

        vmlal.u8   q3,  d31, d19
        vmlal.u8   q2,  d30, d18
        vmlal.u8   q1,  d29, d17
        vmlal.u8   q0,  d28, d16

        vsra.u16   q3,  q3,  #8
        vsra.u16   q2,  q2,  #8
        vsra.u16   q1,  q1,  #8
        vsra.u16   q0,  q0,  #8

        subs       r3,  #8

        vrshrn.u16 d7,  q3,  #8
        vrshrn.u16 d6,  q2,  #8
        vrshrn.u16 d5,  q1,  #8
        vrshrn.u16 d4,  q0,  #8

        vbit       q3,  q12, q12        @ Set alpha to #255
        vbit       q2,  q12, q12

        vst1.32   { q2,q3 }, [r0]!
        bge        1b

2:
        cmp        r3, #-8
        ble        3f

        @ Deal with tail
        @ As the calculation on each pel is independant it doesn't matter
        @ where the pels are in in the registers as long as we are consistant
        @ with what we use for load & store. (Unused register fields will just
        @ calculate garbage that we then ignore.)

        lsls       r2,  r3,  #30        @ b2 -> C, b1 -> N
        vldmcs     r1!, {d2,d3}
        vldmcs     r0!, {d30, d31}
        vldmmi     r1!, {d1}
        vldmmi     r0!, {d29}
        tst        r3,  #1
        vldrne     s1,  [r1]
        vldrne     s9,  [r0]

        @ Alpha in hi byte of each word
        vshr.u32   q9,  q1,  #24
        vshr.u32   q8,  q0,  #24
        vmov       d28, d4              @ Can't load "s57" so cheat
        vmul.u32   q9,  q9,  d8[0]
        vmul.u32   q8,  q8,  d8[0]
        vrshr.u32  q9,  q9,  #16
        vrshr.u32  q8,  q8,  #16        @ 4 mix alpha values
        vmul.u32   q9,  q10
        vmul.u32   q8,  q10             @ dup every alpha into all 4 bytes of its word

        vmull.u8   q3,  d3,  d19
        vmull.u8   q2,  d2,  d18
        vsub.u8    q9,  q11, q9         @ gen the cplmnt

        vmull.u8   q1,  d1,  d17
        vmull.u8   q0,  d0,  d16
        vsub.u8    q8,  q11, q8

        vmlal.u8   q3,  d31, d19
        vmlal.u8   q2,  d30, d18
        vmlal.u8   q1,  d29, d17
        vmlal.u8   q0,  d28, d16

        vsra.u16   q3,  q3,  #8
        vsra.u16   q2,  q2,  #8
        vsra.u16   q1,  q1,  #8
        vsra.u16   q0,  q0,  #8

        vrshrn.u16 d7,  q3,  #8
        vrshrn.u16 d6,  q2,  #8
        vrshrn.u16 d5,  q1,  #8
        vrshrn.u16 d4,  q0,  #8

        vbit       q3,  q12, q12        @ Set alpha to #255
        vbit       q2,  q12, q12

        vstrne     s9,  [r0]
        lsls       r2,  r3,  #30        @ b2 -> C, b1 -> N
        vstmdbmi   r0!, {d5}
        vstmdbcs   r0!, {d6,d7}

3:
        vpop      { q4 }
        bx         lr



