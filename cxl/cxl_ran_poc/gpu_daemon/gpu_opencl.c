#define _GNU_SOURCE
#include "gpu_opencl.h"
#include "cxl_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ──────────────────────────────────────────────────────────────────────────── */
#ifdef HAVE_OPENCL
/* Target OpenCL 2.0 to avoid deprecated clCreateCommandQueue warning */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>

typedef struct {
	cl_platform_id   platform;
	cl_device_id     device;
	cl_context       context;
	cl_command_queue queue;
	cl_program       program;
	cl_kernel        kernel_ldpc;
	cl_kernel        kernel_fft;
	cl_mem           cl_input_buf;
	cl_mem           cl_output_buf;
} cl_internal_t;

static char *load_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[sz] = '\0';
	fclose(f);
	return buf;
}

/* Minimal fallback kernel compiled when opencl_kernels.cl is not found */
static const char *FALLBACK_KERNEL_SRC =
	"__kernel void ldpc_check_node_update("
	"  __global const float *in, __global float *out, const int n) {"
	"  int i = get_global_id(0);"
	"  if (i < n) out[i] = in[i] * 0.75f; }\n"
	"__kernel void ldpc_variable_node_update("
	"  __global float *llr, __global const float *ch, const int n) {"
	"  int i = get_global_id(0);"
	"  if (i < n) llr[i] = llr[i]*0.5f + ch[i%n]; }\n"
	"__kernel void fft_butterfly("
	"  __global float2 *d, const int N, const int s) {"
	"  int i = get_global_id(0);"
	"  if (i < N/2) { float2 a=d[i*2], b=d[i*2+1];"
	"  d[i*2]=(float2)(a.x+b.x,a.y+b.y);"
	"  d[i*2+1]=(float2)(a.x-b.x,a.y-b.y); } }";

int opencl_init(gpu_opencl_ctx_t *ocl, cxl_ctx_t *cxl,
                const char *kernel_path)
{
	memset(ocl, 0, sizeof(*ocl));

	cl_internal_t *cl = calloc(1, sizeof(*cl));
	if (!cl)
		return -1;

	cl_int err;

	/* Platform */
	err = clGetPlatformIDs(1, &cl->platform, NULL);
	if (err != CL_SUCCESS) {
		fprintf(stderr, "[opencl] clGetPlatformIDs: %d\n", err);
		free(cl);
		return -1;
	}

	/* Device: GPU preferred, CPU (PoCL) fallback */
	err = clGetDeviceIDs(cl->platform, CL_DEVICE_TYPE_GPU,
	                     1, &cl->device, NULL);
	if (err != CL_SUCCESS) {
		err = clGetDeviceIDs(cl->platform, CL_DEVICE_TYPE_CPU,
		                     1, &cl->device, NULL);
		if (err != CL_SUCCESS) {
			fprintf(stderr, "[opencl] No device found: %d\n", err);
			free(cl);
			return -1;
		}
		ocl->is_gpu = 0;
	} else {
		ocl->is_gpu = 1;
	}

	clGetDeviceInfo(cl->device, CL_DEVICE_NAME,
	                sizeof(ocl->device_name), ocl->device_name, NULL);
	printf("[opencl] Device: %s (%s)\n",
	       ocl->device_name, ocl->is_gpu ? "GPU" : "PoCL-CPU");

	/* Context */
	cl->context = clCreateContext(NULL, 1, &cl->device, NULL, NULL, &err);
	if (err != CL_SUCCESS) {
		free(cl);
		return -1;
	}

	/* Command queue (clCreateCommandQueueWithProperties for OpenCL ≥ 2.0) */
	cl->queue = clCreateCommandQueueWithProperties(cl->context, cl->device,
	                                                NULL, &err);
	if (err != CL_SUCCESS) {
		clReleaseContext(cl->context);
		free(cl);
		return -1;
	}

	/* Load kernel source */
	char *src = kernel_path ? load_file(kernel_path) : NULL;
	int   using_fallback = (src == NULL);

	if (using_fallback) {
		if (kernel_path)
			fprintf(stderr, "[opencl] Cannot open %s, using fallback kernel\n",
			        kernel_path);
		src = strdup(FALLBACK_KERNEL_SRC);
		if (!src) {
			clReleaseCommandQueue(cl->queue);
			clReleaseContext(cl->context);
			free(cl);
			return -1;
		}
	}

	/* Build program */
	size_t src_len = strlen(src);
	cl->program = clCreateProgramWithSource(cl->context, 1,
	                (const char **)&src, &src_len, &err);
	free(src);

	if (err != CL_SUCCESS) {
		fprintf(stderr, "[opencl] clCreateProgramWithSource: %d\n", err);
		clReleaseCommandQueue(cl->queue);
		clReleaseContext(cl->context);
		free(cl);
		return -1;
	}

	err = clBuildProgram(cl->program, 1, &cl->device, "", NULL, NULL);
	if (err != CL_SUCCESS) {
		char log[8192] = { 0 };

		clGetProgramBuildInfo(cl->program, cl->device,
		                      CL_PROGRAM_BUILD_LOG,
		                      sizeof(log), log, NULL);
		fprintf(stderr, "[opencl] Build failed:\n%s\n", log);
		clReleaseProgram(cl->program);
		clReleaseCommandQueue(cl->queue);
		clReleaseContext(cl->context);
		free(cl);
		return -1;
	}

	cl->kernel_ldpc = clCreateKernel(cl->program,
	                                  "ldpc_check_node_update", &err);
	if (err != CL_SUCCESS)
		fprintf(stderr, "[opencl] ldpc_check_node_update kernel: %d\n", err);

	cl->kernel_fft = clCreateKernel(cl->program,
	                                 "fft_butterfly", &err);
	if (err != CL_SUCCESS)
		fprintf(stderr, "[opencl] fft_butterfly kernel: %d\n", err);

	/* Create OpenCL buffers backed by CXL memory (zero-copy via USE_HOST_PTR) */
	void  *cxl_in  = cxl_get_input_buf(cxl);
	void  *cxl_out = cxl_get_output_buf(cxl);
	size_t in_sz   = CXL_REGION_L1_INPUT_SZ;
	size_t out_sz  = CXL_REGION_GPU_OUTPUT_SZ;

	cl->cl_input_buf = clCreateBuffer(cl->context,
	    CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
	    in_sz, cxl_in, &err);
	if (err == CL_SUCCESS) {
		ocl->zero_copy = 1;
		printf("[opencl] CXL input mapped as OpenCL buffer (CL_MEM_USE_HOST_PTR)\n");
	} else {
		/* PoCL may reject host ptrs that are not page-aligned; fall back */
		cl->cl_input_buf = clCreateBuffer(cl->context,
		    CL_MEM_READ_WRITE, in_sz, NULL, &err);
		ocl->zero_copy = 0;
		if (err != CL_SUCCESS) {
			fprintf(stderr, "[opencl] input buffer alloc failed: %d\n", err);
		}
	}

	cl->cl_output_buf = clCreateBuffer(cl->context,
	    CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
	    out_sz, cxl_out, &err);
	if (err != CL_SUCCESS) {
		cl->cl_output_buf = clCreateBuffer(cl->context,
		    CL_MEM_READ_WRITE, out_sz, NULL, &err);
	}

	ocl->impl      = cl;
	ocl->available = 1;

	printf("[opencl] Init OK  backend=%s  zero_copy=%s\n",
	       ocl->is_gpu ? "GPU" : "PoCL-CPU",
	       ocl->zero_copy ? "yes" : "no");
	return 0;
}

int opencl_compute_ldpc(gpu_opencl_ctx_t *ocl,
                         const struct offload_event *ev)
{
	if (!ocl->available)
		return -1;

	cl_internal_t *cl = ocl->impl;
	if (!cl->kernel_ldpc || !cl->cl_input_buf || !cl->cl_output_buf)
		return -1;

	int n_var   = (ev->input_len > 0) ? (int)ev->input_len : 8448;
	int n_check = (n_var / 4 > 0) ? n_var / 4 : 1;

	if (n_check > 4096)
		n_check = 4096;

	clSetKernelArg(cl->kernel_ldpc, 0, sizeof(cl_mem), &cl->cl_input_buf);
	clSetKernelArg(cl->kernel_ldpc, 1, sizeof(cl_mem), &cl->cl_output_buf);
	clSetKernelArg(cl->kernel_ldpc, 2, sizeof(int),    &n_var);

	size_t gws = (size_t)n_check;
	cl_int err = clEnqueueNDRangeKernel(cl->queue, cl->kernel_ldpc,
	                                     1, NULL, &gws, NULL,
	                                     0, NULL, NULL);
	if (err != CL_SUCCESS)
		return -1;

	clFinish(cl->queue);
	return 0;
}

int opencl_compute_fft(gpu_opencl_ctx_t *ocl,
                        const struct offload_event *ev)
{
	if (!ocl->available)
		return -1;

	cl_internal_t *cl = ocl->impl;
	if (!cl->kernel_fft || !cl->cl_input_buf)
		return -1;

	int N = (ev->input_len > 0) ? (int)(ev->input_len / 8) : 4096;

	if (N < 2)  N = 2;
	if (N > 16384) N = 16384;

	/* Round down to power of 2 */
	int stages = 0, tmp = N;
	while (tmp > 1) { stages++; tmp >>= 1; }
	N = 1 << stages;

	for (int s = 0; s < stages; s++) {
		clSetKernelArg(cl->kernel_fft, 0, sizeof(cl_mem), &cl->cl_input_buf);
		clSetKernelArg(cl->kernel_fft, 1, sizeof(int),    &N);
		clSetKernelArg(cl->kernel_fft, 2, sizeof(int),    &s);

		size_t gws = (size_t)(N / 2);
		cl_int err = clEnqueueNDRangeKernel(cl->queue, cl->kernel_fft,
		                                     1, NULL, &gws, NULL,
		                                     0, NULL, NULL);
		if (err != CL_SUCCESS)
			return -1;
		clFinish(cl->queue);
	}

	return 0;
}

void opencl_fini(gpu_opencl_ctx_t *ocl)
{
	if (!ocl->available || !ocl->impl)
		return;

	cl_internal_t *cl = ocl->impl;

	if (cl->kernel_ldpc)   clReleaseKernel(cl->kernel_ldpc);
	if (cl->kernel_fft)    clReleaseKernel(cl->kernel_fft);
	if (cl->cl_input_buf)  clReleaseMemObject(cl->cl_input_buf);
	if (cl->cl_output_buf) clReleaseMemObject(cl->cl_output_buf);
	if (cl->program)       clReleaseProgram(cl->program);
	if (cl->queue)         clReleaseCommandQueue(cl->queue);
	if (cl->context)       clReleaseContext(cl->context);

	free(cl);
	ocl->impl      = NULL;
	ocl->available = 0;
}

void opencl_log_backend(const gpu_opencl_ctx_t *ocl, const char *out_path)
{
	FILE *f = fopen(out_path, "a");
	if (!f)
		return;

	if (ocl->available) {
		fprintf(f, "gpu_backend: %s\n",
		        ocl->is_gpu ? "real-gpu" : "pocl-cpu");
		fprintf(f, "opencl_device: %s\n", ocl->device_name);
		fprintf(f, "cxl_zero_copy: %s\n",
		        ocl->zero_copy ? "CL_MEM_USE_HOST_PTR" : "copy-path");
		fprintf(f, "paper_note: PoCL CPU backend — swap ICD for AMD ROCm"
		        " or NVIDIA OpenCL for real GPU, zero code changes\n");
	} else {
		fprintf(f, "gpu_backend: fallback-bare-c\n");
		fprintf(f, "paper_note: OpenCL not available at build time;"
		        " bare-C compute used\n");
	}

	fclose(f);
}

/* ──────────────────────────────────────────────────────────────────────────── */
#else /* !HAVE_OPENCL */

int opencl_init(gpu_opencl_ctx_t *ocl, cxl_ctx_t *cxl,
                const char *kernel_path)
{
	(void)cxl;
	(void)kernel_path;
	memset(ocl, 0, sizeof(*ocl));
	printf("[opencl] Not compiled in — using bare-C compute\n");
	return -1;
}

int opencl_compute_ldpc(gpu_opencl_ctx_t *ocl,
                         const struct offload_event *ev)
{
	(void)ocl;
	(void)ev;
	return -1;
}

int opencl_compute_fft(gpu_opencl_ctx_t *ocl,
                        const struct offload_event *ev)
{
	(void)ocl;
	(void)ev;
	return -1;
}

void opencl_fini(gpu_opencl_ctx_t *ocl) { (void)ocl; }

void opencl_log_backend(const gpu_opencl_ctx_t *ocl, const char *out_path)
{
	(void)ocl;
	FILE *f = fopen(out_path, "a");
	if (!f)
		return;
	fprintf(f, "gpu_backend: fallback-bare-c (HAVE_OPENCL not set)\n");
	fclose(f);
}

#endif /* HAVE_OPENCL */
