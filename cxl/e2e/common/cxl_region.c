#define _GNU_SOURCE
#include "cxl_region.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>
#include <numaif.h>
#include <sys/syscall.h>
#include <unistd.h>

static int cxl_node_from_env(void)
{
    const char *env = getenv("CXL_NODE");
    if (!env || !*env) return 0;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || v < 0) {
        fprintf(stderr, "cxl_region: WARN invalid CXL_NODE=%s, defaulting to 0\n", env);
        return 0;
    }
    return (int)v;
}

cxl_region_t *cxl_alloc(size_t bytes)
{
    if (bytes == 0) {
        fprintf(stderr, "cxl_region: ERROR cxl_alloc(0)\n");
        return NULL;
    }
    if (numa_available() < 0) {
        fprintf(stderr, "cxl_region: ERROR NUMA not available on this system\n");
        return NULL;
    }

    int node = cxl_node_from_env();
    int max_node = numa_max_node();
    if (node > max_node) {
        fprintf(stderr,
                "cxl_region: ERROR CXL_NODE=%d requested but max NUMA node is %d "
                "(WSL has 1 node; GCP CXL bring-up must complete before CXL_NODE=1)\n",
                node, max_node);
        return NULL;
    }

    void *ptr = numa_alloc_onnode(bytes, node);
    if (!ptr) {
        fprintf(stderr, "cxl_region: ERROR numa_alloc_onnode(%zu, node=%d) failed\n", bytes, node);
        return NULL;
    }

    cxl_region_t *r = malloc(sizeof(*r));
    if (!r) {
        numa_free(ptr, bytes);
        fprintf(stderr, "cxl_region: ERROR malloc(cxl_region_t) failed\n");
        return NULL;
    }
    r->ptr = ptr;
    r->size = bytes;
    r->req_node = node;

    fprintf(stderr, "cxl_region: alloc base=%p size=%zu CXL_NODE=%d\n", ptr, bytes, node);
    return r;
}

int cxl_verify_node(const cxl_region_t *r)
{
    if (!r || !r->ptr) {
        fprintf(stderr, "cxl_region: ERROR cxl_verify_node(NULL)\n");
        return -1;
    }

    int mode = 0;
    unsigned long nodemask = 0;
    long rc = syscall(SYS_get_mempolicy, &mode, &nodemask, 8UL, r->ptr,
                       MPOL_F_ADDR | MPOL_F_NODE);
    int actual_node = (rc == 0) ? mode : -1;

    fprintf(stderr,
            "cxl_region: region base=%p size=%zu req_node=%d actual_node=%d\n",
            r->ptr, r->size, r->req_node, actual_node);

    if (rc != 0) {
        fprintf(stderr, "cxl_region: WARN get_mempolicy failed errno=%d (%s)\n",
                errno, strerror(errno));
    }
    return actual_node;
}

void cxl_free(cxl_region_t *r)
{
    if (!r) return;
    if (r->ptr) numa_free(r->ptr, r->size);
    free(r);
}
