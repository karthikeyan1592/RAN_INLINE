/* cxl_region.h — CXL-emulated shared-memory region abstraction.
 *
 * Every "CXL memory" access in this pipeline goes through this API. The
 * backend NUMA node is selected once, via the CXL_NODE env var:
 *   CXL_NODE=0 (default) — WSL: single-NUMA box, node 0 is plain DRAM.
 *   CXL_NODE=1            — GCP: CXL surfaced as system-RAM NUMA node 1.
 * No other file may hard-code a NUMA node number (E2E_ARCH_SPEC.md §"Portability
 * requirement"). This is what makes WSL->GCP a config flip, not a rewrite.
 *
 * Backing is always numa_alloc_onnode() over regular system-RAM NUMA — never
 * a /dev/dax* mmap (that path costs ~23 us/byte in QEMU device-memory
 * emulation; see the v8 postmortem, DEV-040).
 */
#ifndef CXL_REGION_H
#define CXL_REGION_H

#include <stddef.h>

typedef struct {
    void   *ptr;         /* base address of the region */
    size_t  size;         /* bytes */
    int     req_node;     /* node requested (from CXL_NODE) */
} cxl_region_t;

/* Allocate `bytes` on the NUMA node selected by CXL_NODE (default 0).
 * Returns NULL and logs to stderr on failure. */
cxl_region_t *cxl_alloc(size_t bytes);

/* Verify actual placement via get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE) on the
 * region's base page. Returns the node number reported by the kernel, or -1
 * on error. Always logs "region base=%p size=%zu req_node=%d actual_node=%d"
 * to stderr so every gate report can cite this line verbatim. */
int cxl_verify_node(const cxl_region_t *r);

/* Free a region allocated by cxl_alloc(). */
void cxl_free(cxl_region_t *r);

#endif /* CXL_REGION_H */
