// Bitwise CRC-32 over 1 MiB of generated data (branch- and shift-heavy).
#include "rt.h"
#define N (1u << 20)
static uint8_t data[N];

int main(void) {
    for (uint32_t i = 0; i < N; i++) data[i] = (uint8_t)(i * 2654435761u >> 24);
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < N; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    print("crc32: ");
    print_hex(crc);
    print("\n");
    return 0;
}
