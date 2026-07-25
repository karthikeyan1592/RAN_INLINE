#ifndef GPU_OPENCL_H
#define GPU_OPENCL_H

#include "cxl_memory.h"
#include "../ebpf/l1_intercept.h"

/*
 * OpenCL compute context for the GPU daemon.
 * Backed by PoCL on CPU; drop-in for AMD ROCm or NVIDIA OpenCL.
 *
 * CXL memory is mapped as CL_MEM_USE_HOST_PTR so the runtime
 * accesses CXL NUMA pages directly (zero-copy).
 */
typedef struct {
	int  available;           /* 1 = OpenCL init OK                */
	char device_name[256];    /* reported by clGetDeviceInfo        */
	int  is_gpu;              /* 1 = real GPU, 0 = PoCL CPU         */
	int  zero_copy;           /* 1 = CL_MEM_USE_HOST_PTR succeeded  */
	void *impl;               /* opaque: cl_internal_t *            */
} gpu_opencl_ctx_t;

/*
 * Initialise OpenCL runtime, compile kernels, bind CXL memory buffers.
 * kernel_path: path to opencl_kernels.cl (absolute or relative to cwd)
 * Returns 0 on success, -1 on failure (caller may use bare-C fallback).
 */
int  opencl_init(gpu_opencl_ctx_t *ocl, cxl_ctx_t *cxl,
                 const char *kernel_path);

/* Enqueue ldpc_check_node_update and clFinish. */
int  opencl_compute_ldpc(gpu_opencl_ctx_t *ocl,
                          const struct offload_event *ev);

/* Enqueue fft_butterfly (all stages) and clFinish. */
int  opencl_compute_fft(gpu_opencl_ctx_t *ocl,
                         const struct offload_event *ev);

/* Release all OpenCL objects. */
void opencl_fini(gpu_opencl_ctx_t *ocl);

/*
 * Append OpenCL backend info to emulation_mode.txt:
 *   gpu_backend: pocl-cpu | real-gpu
 *   opencl_device: <name>
 *   cxl_zero_copy: yes | no
 */
void opencl_log_backend(const gpu_opencl_ctx_t *ocl, const char *out_path);

#endif /* GPU_OPENCL_H */
