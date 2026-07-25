#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * CXL region abstraction — the ONE seam between WSL2 and DO.
 *
 * WSL2:  CXL_BACKING = /tmp/cxl_standin.bin (or env override)
 *        mmap MAP_SHARED of a ftruncate'd file — 4096-aligned guaranteed.
 * DO:    CXL_BACKING = /dev/dax0.0
 *        mmap MAP_SHARED of the DAX device.
 *
 * Everything downstream uses (cxl_base + offset) — no raw pointers.
 */

#define CXL_REGION_SIZE  (256UL * 1024 * 1024)   /* 256 MiB — fits LLR+out */
#define CXL_LLR_OFFSET   (0UL)
#define CXL_OUT_OFFSET   (128UL * 1024 * 1024)   /* 128 MiB into region */

/*
 * Map the CXL region. Reads CXL_BACKING env var to choose backing.
 * Asserts 4096-byte alignment (required by PoCL CL_MEM_USE_HOST_PTR).
 * Prints the chosen backing path and base address to stderr.
 * Returns the base pointer; aborts on failure.
 */
void *cxl_region_map(size_t size, uintptr_t *out_phys_hint);

/* Unmap a previously mapped region. */
void cxl_region_unmap(void *base, size_t size);

/* Return the active backing path string (for logging / gate evidence). */
const char *cxl_region_backing_path(void);
