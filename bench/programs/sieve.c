// Sieve of Eratosthenes: primes below 2,000,000, repeated 3 times.
#include "rt.h"
#define N 2000000
static uint8_t composite[N];

int main(void) {
    uint32_t count = 0;
    for (int rep = 0; rep < 3; rep++) {
        memset(composite, 0, sizeof composite);
        count = 0;
        for (uint32_t i = 2; i < N; i++) {
            if (composite[i]) continue;
            count++;
            for (uint32_t j = i * 2; j < N; j += i) composite[j] = 1;
        }
    }
    print("primes below 2000000: ");
    print_u32(count);
    print("\n");
    return count == 148933 ? 0 : 1;
}
