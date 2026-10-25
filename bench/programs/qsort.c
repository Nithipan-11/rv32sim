// Quicksort 200,000 pseudo-random integers (xorshift32), then verify the order.
#include "rt.h"
#define N 200000
static uint32_t v[N];

static void quicksort(uint32_t* x, int lo, int hi) {
    while (lo < hi) {
        uint32_t p = x[(lo + hi) >> 1];
        int i = lo, j = hi;
        while (i <= j) {
            while (x[i] < p) i++;
            while (x[j] > p) j--;
            if (i <= j) { uint32_t t = x[i]; x[i] = x[j]; x[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quicksort(x, lo, j); lo = i; }
        else { quicksort(x, i, hi); hi = j; }
    }
}

int main(void) {
    uint32_t s = 2463534242u;
    for (int i = 0; i < N; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; v[i] = s; }
    quicksort(v, 0, N - 1);
    for (int i = 1; i < N; i++)
        if (v[i - 1] > v[i]) { print("NOT SORTED\n"); return 1; }
    print("sorted 200000 values, min ");
    print_hex(v[0]);
    print(" max ");
    print_hex(v[N - 1]);
    print("\n");
    return 0;
}
