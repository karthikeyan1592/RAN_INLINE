#include "cxl_region.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#define STANDIN_DEFAULT "/tmp/cxl_standin.bin"

static char g_backing_path[256] = "";

const char *cxl_region_backing_path(void) { return g_backing_path; }

void *cxl_region_map(size_t size, uintptr_t *out_phys_hint)
{
    const char *backing = getenv("CXL_BACKING");
    if (!backing || backing[0] == '\0')
        backing = STANDIN_DEFAULT;

    strncpy(g_backing_path, backing, sizeof(g_backing_path) - 1);

    int is_dax = (strncmp(backing, "/dev/dax", 8) == 0);

    int fd = open(backing, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        fprintf(stderr, "[cxl_region] open(%s) failed: %s\n",
                backing, strerror(errno));
        abort();
    }

    if (!is_dax) {
        if (ftruncate(fd, (off_t)size) < 0) {
            fprintf(stderr, "[cxl_region] ftruncate(%zu) failed: %s\n",
                    size, strerror(errno));
            abort();
        }
    }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    close(fd);

    if (base == MAP_FAILED) {
        fprintf(stderr, "[cxl_region] mmap(%zu, %s) failed: %s\n",
                size, backing, strerror(errno));
        abort();
    }

    assert(((uintptr_t)base % 4096) == 0 &&
           "cxl_region_map: base not 4096-aligned — PoCL CL_MEM_USE_HOST_PTR will fail");

    fprintf(stderr, "[cxl_region] backing=%s  base=%p  size=%zu MiB  %s\n",
            backing, base, size >> 20,
            is_dax ? "REAL-DAX" : "STAND-IN");

    if (out_phys_hint)
        *out_phys_hint = (uintptr_t)base;  /* hint only; not a real phys addr */

    return base;
}

void cxl_region_unmap(void *base, size_t size)
{
    munmap(base, size);
}
