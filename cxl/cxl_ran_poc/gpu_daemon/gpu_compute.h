#ifndef GPU_COMPUTE_H
#define GPU_COMPUTE_H

#include "../ebpf/l1_intercept.h"
#include "cxl_memory.h"

int gpu_compute_ldpc(cxl_ctx_t *cxl, const struct offload_event *event);
int gpu_compute_fft(cxl_ctx_t *cxl, const struct offload_event *event);

#endif /* GPU_COMPUTE_H */
