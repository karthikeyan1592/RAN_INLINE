#define _GNU_SOURCE
#include "../ebpf/l1_intercept.h"
#include "../gpu_daemon/cxl_memory.h"
#include "../l1_sim/fft.h"
#include "../l1_sim/ldpc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static cxl_ctx_t g_cxl;
static int g_cxl_ready;
static int g_async_mode;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/*
 * Async double-buffer state.
 *
 * In async mode the caller never spin-waits on the daemon.  Instead:
 *   - Call N: dispatch work, return previous slot's result immediately.
 *   - Call N+1: dispatch work, return slot-N result immediately.
 * The cached result is updated whenever the daemon signals gpu_done
 * during a brief (≤1 ms) opportunistic check before the next dispatch.
 *
 * First call is always synchronous to prime the pipeline.
 */
#define ASYNC_BUF_SZ 8448

static uint8_t g_async_result[ASYNC_BUF_SZ];
static size_t  g_async_result_len;
static int     g_async_has_result;  /* 1 after first sync call completes */

static void init_cxl(void)
{
	const char *path = getenv("CXL_DEV_PATH");

	if (!path)
		path = "/dev/dax0.0";

	if (cxl_init(&g_cxl, path) < 0)
		cxl_init_shm_fallback(&g_cxl);
	g_cxl_ready = 1;
}

static int send_offload_event(const struct offload_event *event)
{
	const char *sock_path = getenv("GPU_DAEMON_SOCKET");
	int fd;
	struct sockaddr_un addr;

	if (!sock_path)
		sock_path = "/tmp/gpu_daemon.sock";

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -errno;
	}

	if (write(fd, event, sizeof(*event)) != (ssize_t)sizeof(*event)) {
		close(fd);
		return -EIO;
	}
	close(fd);
	return 0;
}

static int wait_gpu_done(uint32_t timeout_ms)
{
	cxl_ctrl_block_t *ctrl = cxl_get_ctrl(&g_cxl);
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!ctrl->gpu_done) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t elapsed_ms = (uint64_t)(now.tv_sec - start.tv_sec) * 1000ULL +
				      (uint64_t)(now.tv_nsec - start.tv_nsec) / 1000000ULL;

		if (elapsed_ms > timeout_ms)
			return -ETIMEDOUT;
		usleep(50);
	}
	return 0;
}

/*
 * Dispatch LDPC work to daemon and fill in the ctrl block.
 * Does NOT wait for completion; caller decides whether to spin.
 */
static int dispatch_ldpc(const ldpc_params_t *p)
{
	cxl_ctrl_block_t *ctrl = cxl_get_ctrl(&g_cxl);
	struct timespec ts;

	ctrl->gpu_done  = 0;
	ctrl->l1_ready  = 0;
	ctrl->work_type = WORK_TYPE_LDPC;

	memcpy(cxl_get_input_buf(&g_cxl), p->input, p->input_len);
	ctrl->input_offset  = CXL_REGION_L1_INPUT_OFF;
	ctrl->input_len     = p->input_len;
	ctrl->output_offset = CXL_REGION_GPU_OUTPUT_OFF;
	ctrl->output_len    = p->output_len;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ctrl->timestamp_l1 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	ctrl->l1_ready = 1;

	struct offload_event ev = {
		.work_type   = WORK_TYPE_LDPC,
		.pid         = (uint32_t)getpid(),
		.input_addr  = CXL_REGION_L1_INPUT_OFF,
		.output_addr = CXL_REGION_GPU_OUTPUT_OFF,
		.input_len   = ctrl->input_len,
		.output_len  = ctrl->output_len,
	};
	return send_offload_event(&ev);
}

/*
 * Async LDPC offload (pipeline double-buffer):
 *
 *   Slot N:   return prev result → opportunistic collect → dispatch new work
 *   Slot N+1: return slot-N result → opportunistic collect → dispatch new work
 *   ...
 *
 * The first call is synchronous to prime the pipeline.
 */
static int l1_offload_async_ldpc(const ldpc_params_t *p)
{
	cxl_ctrl_block_t *ctrl = cxl_get_ctrl(&g_cxl);

	if (!g_async_has_result) {
		/* Prime: synchronous first call */
		if (dispatch_ldpc(p) < 0)
			return -1;
		if (wait_gpu_done(500) < 0)
			return -1;

		size_t out_len = (size_t)ctrl->output_len;

		memcpy(p->output, cxl_get_output_buf(&g_cxl), out_len);
		if (out_len > ASYNC_BUF_SZ)
			out_len = ASYNC_BUF_SZ;
		memcpy(g_async_result, p->output, out_len);
		g_async_result_len  = out_len;
		g_async_has_result  = 1;

		/* Immediately dispatch the NEXT slot's work — daemon is now idle */
		dispatch_ldpc(p);   /* fire and forget; result arrives before next call */
		return 0;
	}

	/*
	 * Fast path: return the cached result from the previous dispatch.
	 * Then do a brief opportunistic spin (≤1 ms) to collect the result
	 * of the work submitted in the previous call so we can update our cache.
	 */
	size_t copy_len = g_async_result_len < p->output_len ?
	                  g_async_result_len : p->output_len;
	memcpy(p->output, g_async_result, copy_len);

	/* Opportunistic collect: spin ≤1 ms */
	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	uint64_t deadline_ns = (uint64_t)deadline.tv_sec * 1000000000ULL +
	                       deadline.tv_nsec + 1000000ULL;  /* +1 ms */
	struct timespec now;

	while (!ctrl->gpu_done) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec >= deadline_ns)
			break;
		/* tight spin — let CPU do other work between checks if desired */
	}
	if (ctrl->gpu_done) {
		size_t out_len = (size_t)ctrl->output_len;

		if (out_len > ASYNC_BUF_SZ)
			out_len = ASYNC_BUF_SZ;
		memcpy(g_async_result, cxl_get_output_buf(&g_cxl), out_len);
		g_async_result_len = out_len;
	}

	/* Dispatch new work non-blocking — we do NOT wait */
	dispatch_ldpc(p);
	return 0;
}

int l1_offload_try(uint32_t work_type, const void *params)
{
	/* Read env vars lazily so setenv() in measure.c takes effect */
	const char *env = getenv("RAN_OFFLOAD");

	if (!env || strcmp(env, "1") != 0)
		return -1;

	const char *aenv = getenv("ASYNC_OFFLOAD");

	g_async_mode = aenv && strcmp(aenv, "1") == 0;

	pthread_once(&g_once, init_cxl);
	if (!g_cxl_ready)
		return -1;

	if (g_async_mode && work_type == WORK_TYPE_LDPC)
		return l1_offload_async_ldpc(params);

	/* ── Synchronous path (original architecture) ──────────────────────── */
	cxl_ctrl_block_t *ctrl = cxl_get_ctrl(&g_cxl);

	ctrl->gpu_done = 0;
	ctrl->l1_ready = 0;
	ctrl->work_type = work_type;

	if (work_type == WORK_TYPE_LDPC) {
		const ldpc_params_t *p = params;

		memcpy(cxl_get_input_buf(&g_cxl), p->input, p->input_len);
		ctrl->input_offset = CXL_REGION_L1_INPUT_OFF;
		ctrl->input_len = p->input_len;
		ctrl->output_offset = CXL_REGION_GPU_OUTPUT_OFF;
		ctrl->output_len = p->output_len;
	} else if (work_type == WORK_TYPE_FFT) {
		const fft_params_t *p = params;
		size_t bytes = p->N * sizeof(float complex);

		memcpy(cxl_get_input_buf(&g_cxl), p->input, bytes);
		ctrl->input_offset = CXL_REGION_L1_INPUT_OFF;
		ctrl->input_len = bytes;
		ctrl->output_offset = CXL_REGION_GPU_OUTPUT_OFF;
		ctrl->output_len = bytes;
	} else {
		return -1;
	}

	struct offload_event ev = {
		.work_type = work_type,
		.pid = (uint32_t)getpid(),
		.input_addr = CXL_REGION_L1_INPUT_OFF,
		.output_addr = CXL_REGION_GPU_OUTPUT_OFF,
		.input_len = ctrl->input_len,
		.output_len = ctrl->output_len,
	};

	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ctrl->timestamp_l1 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	ctrl->l1_ready = 1;

	if (send_offload_event(&ev) < 0)
		return -1;

	if (wait_gpu_done(500) < 0)
		return -1;

	if (work_type == WORK_TYPE_LDPC) {
		const ldpc_params_t *p = params;

		memcpy(p->output, cxl_get_output_buf(&g_cxl), p->output_len);
	} else if (work_type == WORK_TYPE_FFT) {
		const fft_params_t *p = params;

		memcpy(p->output, cxl_get_output_buf(&g_cxl),
		       p->N * sizeof(float complex));
	}

	return 0;
}

