#define _GNU_SOURCE
#include "cxl_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void cxl_setup_ctrl(cxl_ctx_t *ctx)
{
	ctx->ctrl = (cxl_ctrl_block_t *)((uint8_t *)ctx->base + CXL_REGION_CTRL_OFF);
	memset(ctx->ctrl, 0, sizeof(*ctx->ctrl));
}

#define CXL_SHM_PATH "/tmp/cxl_ran_poc_shm"

/*
 * Read the NUMA node that a DAX device exposes in system-ram mode from sysfs.
 * Returns the node number, or -1 if unavailable.
 */
static int cxl_detect_system_ram_node(const char *dev_path)
{
	const char *dev_name = strrchr(dev_path, '/');

	dev_name = dev_name ? dev_name + 1 : dev_path;

	char sysfs[256];

	snprintf(sysfs, sizeof(sysfs),
		 "/sys/bus/dax/devices/%s/target_node", dev_name);

	FILE *f = fopen(sysfs, "r");

	if (!f)
		return -1;
	int node = -1;

	if (fscanf(f, "%d", &node) != 1)
		node = -1;
	fclose(f);
	return node;
}

int cxl_init_shm_fallback(cxl_ctx_t *ctx)
{
	int fd = open(CXL_SHM_PATH, O_RDWR | O_CREAT, 0600);

	if (fd < 0)
		return -errno;

	if (ftruncate(fd, CXL_REGION_TOTAL_SIZE) < 0) {
		close(fd);
		return -errno;
	}

	ctx->fd = fd;
	ctx->base = mmap(NULL, CXL_REGION_TOTAL_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	if (ctx->base == MAP_FAILED) {
		close(fd);
		return -errno;
	}

	ctx->size = CXL_REGION_TOTAL_SIZE;
	ctx->numa_node = -1;
	snprintf(ctx->mode, sizeof(ctx->mode), "mmap-shm-fallback");
	cxl_setup_ctrl(ctx);
	printf("[CXL] Using file-backed shared mmap fallback\n");
	return 0;
}

/*
 * Shared file-backed mapping with pages bound to a specific NUMA node.
 * Used when the DAX device is in system-ram mode (CXL memory appears as a
 * real NUMA node and cannot be directly mmap'd from the char device).
 * Both L1 sim and GPU daemon map the same file, so they share the physical
 * CXL pages on NUMA node `numa_node`.
 */
int cxl_init_shared_cxl_numa(cxl_ctx_t *ctx, int numa_node)
{
	int fd = open(CXL_SHM_PATH, O_RDWR | O_CREAT, 0600);

	if (fd < 0)
		return -errno;

	if (ftruncate(fd, CXL_REGION_TOTAL_SIZE) < 0) {
		close(fd);
		return -errno;
	}

	ctx->fd = fd;
	ctx->base = mmap(NULL, CXL_REGION_TOTAL_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	if (ctx->base == MAP_FAILED) {
		close(fd);
		return -errno;
	}

	/*
	 * Set NUMA policy to MPOL_PREFERRED for node `numa_node` so future
	 * page faults allocate on the CXL node.  Use MPOL_PREFERRED instead
	 * of MPOL_BIND to avoid forcing page migration that can leave pages
	 * in an intermediate state incompatible with AVX stores (SIGILL
	 * on vmovdqu into a migrating page).
	 */
	unsigned long nodemask = 1UL << numa_node;

	if (mbind(ctx->base, CXL_REGION_TOTAL_SIZE, MPOL_PREFERRED,
		  &nodemask, sizeof(nodemask) * 8, 0) < 0) {
		fprintf(stderr, "[CXL] WARN: mbind to node %d failed (%s); "
			"shared mapping will be used without NUMA preference\n",
			numa_node, strerror(errno));
		snprintf(ctx->mode, sizeof(ctx->mode),
			 "qemu-cxl-system-ram-unbnd");
	} else {
		snprintf(ctx->mode, sizeof(ctx->mode),
			 "qemu-cxl-system-ram-node%d", numa_node);
		printf("[CXL] Shared mmap with NUMA node %d preference (CXL)\n",
		       numa_node);
	}

	ctx->size = CXL_REGION_TOTAL_SIZE;
	ctx->numa_node = numa_node;
	cxl_setup_ctrl(ctx);
	return 0;
}

int cxl_init_numa_emulation(cxl_ctx_t *ctx, int numa_node)
{
	if (numa_available() < 0)
		return cxl_init_shm_fallback(ctx);

	ctx->fd = -1;
	ctx->base = mmap(NULL, CXL_REGION_TOTAL_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ctx->base == MAP_FAILED)
		return -errno;

	unsigned long nodemask = 1UL << numa_node;

	if (mbind(ctx->base, CXL_REGION_TOTAL_SIZE, MPOL_BIND,
		  &nodemask, sizeof(nodemask) * 8,
		  MPOL_MF_MOVE | MPOL_MF_STRICT) < 0) {
		munmap(ctx->base, CXL_REGION_TOTAL_SIZE);
		return cxl_init_shm_fallback(ctx);
	}

	ctx->size = CXL_REGION_TOTAL_SIZE;
	ctx->numa_node = numa_node;
	snprintf(ctx->mode, sizeof(ctx->mode), "numa-emulated-node%d", numa_node);
	cxl_setup_ctrl(ctx);
	printf("[CXL] Using NUMA node %d emulation\n", numa_node);
	return 0;
}

int cxl_init(cxl_ctx_t *ctx, const char *dev_path)
{
	memset(ctx, 0, sizeof(*ctx));

	if (dev_path) {
		ctx->fd = open(dev_path, O_RDWR);
		if (ctx->fd >= 0) {
			ctx->base = mmap(NULL, CXL_REGION_TOTAL_SIZE,
					 PROT_READ | PROT_WRITE, MAP_SHARED,
					 ctx->fd, 0);
			if (ctx->base != MAP_FAILED) {
				/* devdax mode: direct device mapping */
				ctx->size = CXL_REGION_TOTAL_SIZE;
				snprintf(ctx->mode, sizeof(ctx->mode),
					 "qemu-cxl-type3");
				cxl_setup_ctrl(ctx);
				printf("[CXL] Using device: %s\n", dev_path);
				return 0;
			}
			close(ctx->fd);
			ctx->fd = -1;
		}

		/*
		 * open() or mmap() failed.  Check sysfs: if the device is in
		 * system-ram mode the kernel owns the pages as a NUMA node and
		 * open() returns ENXIO.  Detect the target NUMA node and use a
		 * shared file-backed mapping bound to it so both the L1 sim
		 * and the GPU daemon land on real CXL memory pages.
		 */
		int cxl_node = cxl_detect_system_ram_node(dev_path);

		if (cxl_node >= 0 &&
		    cxl_init_shared_cxl_numa(ctx, cxl_node) == 0)
			return 0;
	}

	/* Shared mapping required for L1 + GPU daemon; prefer shm over private NUMA */
	if (cxl_init_shm_fallback(ctx) == 0)
		return 0;

	return cxl_init_numa_emulation(ctx, 1);
}

void cxl_fini(cxl_ctx_t *ctx)
{
	if (!ctx || !ctx->base)
		return;

	munmap(ctx->base, ctx->size);
	if (ctx->fd >= 0)
		close(ctx->fd);
	memset(ctx, 0, sizeof(*ctx));
}

void *cxl_get_input_buf(cxl_ctx_t *ctx)
{
	return (uint8_t *)ctx->base + CXL_REGION_L1_INPUT_OFF;
}

void *cxl_get_output_buf(cxl_ctx_t *ctx)
{
	return (uint8_t *)ctx->base + CXL_REGION_GPU_OUTPUT_OFF;
}

cxl_ctrl_block_t *cxl_get_ctrl(cxl_ctx_t *ctx)
{
	return ctx->ctrl;
}
