# Bare-metal entry: set up the stack, zero .bss, call main, exit with its return value.
    .section .text.init
    .globl _start
_start:
    la sp, _stack_top
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
    li a7, 93          # exit(a0)
    ecall
3:  j 3b
