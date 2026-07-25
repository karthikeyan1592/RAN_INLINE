/* test_cxl_region.c — Gate 0 acceptance test for the cxl_region abstraction.
 *
 * Allocates a region, writes a known pattern, reads it back, and verifies
 * NUMA placement via get_mempolicy. Exits 0 on PASS, 1 on FAIL.
 */
#include "../common/cxl_region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SIZE (1u << 20)  /* 1 MB */

int main(void)
{
    int failed = 0;

    printf("[test_cxl_region] alloc %u bytes...\n", TEST_SIZE);
    cxl_region_t *r = cxl_alloc(TEST_SIZE);
    if (!r) {
        printf("[test_cxl_region] FAIL: cxl_alloc returned NULL\n");
        return 1;
    }

    /* write known pattern */
    unsigned char *buf = (unsigned char *)r->ptr;
    for (size_t i = 0; i < r->size; i++) buf[i] = (unsigned char)(i & 0xFF);

    /* read back and verify */
    size_t mismatches = 0;
    for (size_t i = 0; i < r->size; i++) {
        if (buf[i] != (unsigned char)(i & 0xFF)) mismatches++;
    }
    printf("[test_cxl_region] write/read-back: %zu mismatches / %zu bytes\n",
           mismatches, r->size);
    if (mismatches != 0) failed = 1;

    /* verify NUMA placement */
    int actual_node = cxl_verify_node(r);
    printf("[test_cxl_region] requested node=%d actual node (get_mempolicy)=%d\n",
           r->req_node, actual_node);
    if (actual_node != r->req_node) {
        printf("[test_cxl_region] FAIL: actual_node(%d) != req_node(%d)\n",
               actual_node, r->req_node);
        failed = 1;
    }

    cxl_free(r);

    if (failed) {
        printf("[test_cxl_region] OVERALL: FAIL\n");
        return 1;
    }
    printf("[test_cxl_region] OVERALL: PASS\n");
    return 0;
}
