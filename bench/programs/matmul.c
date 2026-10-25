// 96x96 integer matrix multiply, repeated 5 times (exercises the M extension).
#include "rt.h"
#define N 96
static int32_t a[N][N], b[N][N], c[N][N];

int main(void) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = (i * 7 + j * 3) % 17 - 8;
            b[i][j] = (i * 5 + j * 11) % 13 - 6;
        }
    uint32_t checksum = 0;
    for (int rep = 0; rep < 5; rep++) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                int32_t s = 0;
                for (int k = 0; k < N; k++) s += a[i][k] * b[k][j];
                c[i][j] = s + rep;
            }
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) checksum = checksum * 31u + (uint32_t)c[i][j];
    }
    print("matmul checksum: ");
    print_hex(checksum);
    print("\n");
    return 0;
}
