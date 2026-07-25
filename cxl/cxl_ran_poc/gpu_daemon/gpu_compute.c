#include "gpu_compute.h"

#include "../l1_sim/fft.h"
#include "../l1_sim/ldpc.h"

#include <string.h>

static void *cxl_ptr(cxl_ctx_t *cxl, uint64_t addr)
{
	uintptr_t off = (uintptr_t)(addr & 0x1FFFFFFFUL);

	if (off < cxl->size)
		return (uint8_t *)cxl->base + off;
	return cxl_get_input_buf(cxl);
}

int gpu_compute_ldpc(cxl_ctx_t *cxl, const struct offload_event *event)
{
	void *input = cxl_ptr(cxl, event->input_addr);
	void *output = cxl_get_output_buf(cxl);

	ldpc_params_t params = {
		.input = input,
		.output = output,
		.input_len = event->input_len ? event->input_len : 8448,
		.output_len = event->output_len ? event->output_len : 8448,
		.base_graph = 1,
		.lifting_size = 384,
		.code_rate = 0.5f,
	};

	return ldpc_decode_internal(&params);
}

int gpu_compute_fft(cxl_ctx_t *cxl, const struct offload_event *event)
{
	void *input = cxl_ptr(cxl, event->input_addr);
	void *output = cxl_get_output_buf(cxl);
	size_t N = event->input_len / sizeof(float complex);

	if (N == 0)
		N = 4096;

	fft_params_t params = {
		.input = input,
		.output = output,
		.N = N,
		.direction = -1,
		.normalized = 1,
	};

	return fft_process_internal(&params);
}
