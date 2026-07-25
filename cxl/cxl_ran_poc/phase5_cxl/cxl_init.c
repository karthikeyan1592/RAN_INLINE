/* cxl_init.c — LD_PRELOAD constructor: maps CXL region inside the benchmark
 * process (after execl replaces the address space) and writes the benchmark's
 * own VA to a tmpfile so the consumer can update config_map before attaching
 * the uprobe.
 *
 * Required env vars (set by llr_consumer_v8 before fork):
 *   CXL_SHM_NAME     — POSIX shm name, e.g. "/cxl_region_v8"
 *   CXL_REGION_SIZE  — bytes, e.g. "67108864"
 *   CXL_VA_FILE      — tmpfile path, e.g. "/tmp/cxl_va_v8.bin"
 *
 * Build:
 *   gcc -O2 -shared -fPIC -D_GNU_SOURCE cxl_init.c -o cxl_init.so \
 *       -lnuma -lrt
 *
 * Usage:
 *   LD_PRELOAD=libbpftime-agent.so:./cxl_init.so ./ldpc_decoder_benchmark ...
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <numa.h>
#include <numaif.h>

#define CXL_NODE 1

/* Runs after bpftime-agent (which uses no-priority constructors).
 * Priority 200 = runs before no-arg constructors, after implementation ones.
 * Using no-arg here so it runs AFTER all prioritized library constructors,
 * including bpftime-agent. */
static void __attribute__((constructor)) cxl_region_init(void)
{
    const char *shm_name  = getenv("CXL_SHM_NAME");
    const char *size_s    = getenv("CXL_REGION_SIZE");
    const char *va_file   = getenv("CXL_VA_FILE");

    if (!shm_name || !size_s || !va_file) {
        /* Not in the CXL pipeline — skip silently */
        return;
    }

    size_t region_sz = (size_t)atol(size_s);
    if (region_sz == 0) {
        fprintf(stderr, "[cxl_init] bad CXL_REGION_SIZE\n");
        return;
    }

    /* Open the named shared memory (created by the consumer) */
    int sfd = shm_open(shm_name, O_RDWR, 0600);
    if (sfd < 0) {
        perror("[cxl_init] shm_open");
        return;
    }

    /* Map it — this process's VA may differ from the consumer's VA */
    void *base = mmap(NULL, region_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED, sfd, 0);
    close(sfd);
    if (base == MAP_FAILED) {
        perror("[cxl_init] mmap");
        return;
    }

    /* Bind to CXL NUMA node 1 */
    if (numa_available() >= 0 && numa_max_node() >= CXL_NODE) {
        unsigned long nodemask = (1UL << CXL_NODE);
        if (mbind(base, region_sz, MPOL_BIND, &nodemask,
                  sizeof(nodemask) * 8, MPOL_MF_MOVE) != 0) {
            /* Non-fatal: region is already bound by consumer's mbind */
        }
    }

    /* Write this process's VA to tmpfile so consumer can update config_map */
    uint64_t va = (uint64_t)(uintptr_t)base;
    int fd = open(va_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        perror("[cxl_init] open va_file");
        return;
    }
    if (write(fd, &va, sizeof(va)) != sizeof(va)) {
        perror("[cxl_init] write va");
    }
    close(fd);

    fprintf(stderr, "[cxl_init] CXL region mapped at %p (%zu MB), VA written to %s\n",
            base, region_sz >> 20, va_file);
}
