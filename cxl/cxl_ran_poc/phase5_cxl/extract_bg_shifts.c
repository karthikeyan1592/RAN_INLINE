/* extract_bg_shifts.c — extract BG1/Z=384 shift table as flat binary for gate2_e2e
 *
 * Reads BG1_SHIFTS[liftidx][cn][vn] from bg_tables.h and writes
 * N_CN*N_VN_FULL uint16 values to stdout (or to file).
 *
 * Usage:
 *   gcc -I../gpu_daemon/ldpc_cl extract_bg_shifts.c -o extract_bg_shifts
 *   ./extract_bg_shifts 384 > /tmp/bg1_z384_shifts.bin
 *
 * The output is exactly 46*68*2 = 6256 bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "bg_tables.h"  /* BG1_SHIFTS[8][46][68], LS_TO_IDX[385] */

int main(int argc, char **argv) {
    int ls = (argc >= 2) ? atoi(argv[1]) : 384;
    if (ls < 2 || ls > 384) { fprintf(stderr, "LS must be 2..384\n"); return 1; }

    unsigned char idx = LS_TO_IDX[ls];
    if (idx == 255) { fprintf(stderr, "LS=%d: not a valid lifting size\n", ls); return 1; }

    fprintf(stderr, "LS=%d  liftidx=%u  writing %d uint16s\n",
            ls, (unsigned)idx, BG1_M * BG1_N);

    /* Write [cn][vn] flat, uint16 little-endian */
    for (int cn = 0; cn < BG1_M; cn++) {
        for (int vn = 0; vn < BG1_N; vn++) {
            uint16_t v = BG1_SHIFTS[idx][cn][vn];
            fwrite(&v, sizeof(v), 1, stdout);
        }
    }
    return 0;
}
