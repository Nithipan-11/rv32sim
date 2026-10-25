// Minimal freestanding runtime: host-call I/O and the few libc symbols GCC may emit.
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline long sys_write(int fd, const void* buf, size_t n) {
    register long a0 asm("a0") = fd;
    register long a1 asm("a1") = (long)buf;
    register long a2 asm("a2") = (long)n;
    register long a7 asm("a7") = 64;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

void print(const char* s);
void print_u32(uint32_t v);
void print_hex(uint32_t v);
void* memset(void* d, int c, size_t n);
void* memcpy(void* d, const void* s, size_t n);
