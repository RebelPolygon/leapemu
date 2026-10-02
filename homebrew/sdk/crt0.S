; Leapster homebrew start-up: own stack, copy .data, clear .bss, call main.
    .section .text.start, "ax"
    .global _start
_start:
    mov     sp, __stack_top
    mov     r0, __data_start
    mov     r1, __data_end
    mov     r2, __data_load
1:  brhs    r0, r1, 2f
    ld.ab   r3, [r2, 4]
    st.ab   r3, [r0, 4]
    b       1b
2:  mov     r0, __bss_start
    mov     r1, __bss_end
    mov     r3, 0
3:  brhs    r0, r1, 4f
    st.ab   r3, [r0, 4]
    b       3b
4:  bl      main
5:  b       5b
