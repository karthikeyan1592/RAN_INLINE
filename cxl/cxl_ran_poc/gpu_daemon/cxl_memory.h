#ifndef CXL_MEMORY_H
#define CXL_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define CXL_REGION_TOTAL_SIZE     (2UL * 1024 * 1024 * 1024)
#define CXL_REGION_L1_INPUT_OFF   0x00000000
#define CXL_REGION_L1_INPUT_SZ    (256UL * 1024 * 1024)
#define CXL_REGION_GPU_OUTPUT_OFF 0x10000000
#define CXL_REGION_GPU_OUTPUT_SZ  (256UL * 1024 * 1024)
#define CXL_REGION_CTRL_OFF       0x20000000
#define CXL_REGION_CTRL_SZ        (1UL * 1024 * 1024)

typedef struct __attribute__((packed)) {
	volatile uint32_t l1_ready;
	volatile uint32_t gpu_done;
	volatile uint32_t work_type;
	volatile uint32_t input_offset;
	volatile uint32_t input_len;
	volatile uint32_t output_offset;
	volatile uint32_t output_len;
	volatile uint64_t timestamp_l1;
	volatile uint64_t timestamp_gpu;
	uint8_t           _pad[64];
} cxl_ctrl_block_t;

typedef struct {
	void    *base;
	size_t   size;
	int      fd;
	int      numa_node;
	cxl_ctrl_block_t *ctrl;
	char     mode[64];
} cxl_ctx_t;

int  cxl_init(cxl_ctx_t *ctx, const char *dev_path);
void cxl_fini(cxl_ctx_t *ctx);
void *cxl_get_input_buf(cxl_ctx_t *ctx);
void *cxl_get_output_buf(cxl_ctx_t *ctx);
cxl_ctrl_block_t *cxl_get_ctrl(cxl_ctx_t *ctx);
int  cxl_init_numa_emulation(cxl_ctx_t *ctx, int numa_node);
int  cxl_init_shared_cxl_numa(cxl_ctx_t *ctx, int numa_node);
int  cxl_init_shm_fallback(cxl_ctx_t *ctx);

#endif /* CXL_MEMORY_H */
