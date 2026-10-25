#include "rt.h"

void print(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

void print_u32(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    print(&buf[i]);
}

void print_hex(uint32_t v) {
    char buf[11] = "0x";
    for (int i = 0; i < 8; i++) {
        uint32_t d = (v >> (28 - 4 * i)) & 0xF;
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[10] = 0;
    print(buf);
}

void* memset(void* d, int c, size_t n) {
    unsigned char* p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* p = d;
    const unsigned char* q = s;
    while (n--) *p++ = *q++;
    return d;
}
