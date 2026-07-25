#define _GNU_SOURCE
#include "cxl_memory.h"
#include "gpu_compute.h"
#include "gpu_opencl.h"

#include "../ebpf/l1_intercept.h"

/*
 * gpu_daemon — OpenCL compute daemon for the CXL RAN PoC.
 *
 * Initialises CXL shared memory and, when compiled with -DHAVE_OPENCL,
 * binds it as CL_MEM_USE_HOST_PTR OpenCL buffers (zero-copy path).
 * Falls back to bare-C compute when OpenCL is unavailable.
 */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GPU_DAEMON_SOCKET "/tmp/gpu_daemon.sock"
#define MAX_QUEUE_DEPTH   64

typedef struct {
	int sock_listen;
	int sock_client;
	cxl_ctx_t cxl;
	gpu_opencl_ctx_t ocl;           /* OpenCL / PoCL context */
	const char *kernel_path;        /* path to opencl_kernels.cl */
	const char *emulation_log;      /* path to emulation_mode.txt */
	pthread_t worker_thread;
	pthread_mutex_t queue_mutex;
	pthread_cond_t queue_cond;
	struct offload_event queue[MAX_QUEUE_DEPTH];
	int queue_head;
	int queue_tail;
	int queue_count;
	volatile int running;
	int verbose;
	uint64_t processed_ldpc;
	uint64_t processed_fft;
	uint64_t total_latency_ns;
} gpu_daemon_ctx_t;

static gpu_daemon_ctx_t g_ctx;

static uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b)
{
	return (uint64_t)(b->tv_sec - a->tv_sec) * 1000000000ULL +
	       (uint64_t)(b->tv_nsec - a->tv_nsec);
}

static void signal_completion(cxl_ctx_t *cxl, const struct offload_event *event,
			      uint64_t lat_ns)
{
	cxl_ctrl_block_t *ctrl = cxl_get_ctrl(cxl);

	ctrl->gpu_done = 1;
	ctrl->timestamp_gpu = lat_ns;
	(void)event;
}

static void *gpu_worker_thread(void *arg)
{
	gpu_daemon_ctx_t *ctx = arg;

	while (ctx->running) {
		struct offload_event event;

		pthread_mutex_lock(&ctx->queue_mutex);
		while (ctx->queue_count == 0 && ctx->running)
			pthread_cond_wait(&ctx->queue_cond, &ctx->queue_mutex);
		if (!ctx->running) {
			pthread_mutex_unlock(&ctx->queue_mutex);
			break;
		}

		event = ctx->queue[ctx->queue_head];
		ctx->queue_head = (ctx->queue_head + 1) % MAX_QUEUE_DEPTH;
		ctx->queue_count--;
		pthread_mutex_unlock(&ctx->queue_mutex);

		struct timespec t_start, t_end;

		clock_gettime(CLOCK_MONOTONIC, &t_start);

		cxl_ctrl_block_t *ctrl = cxl_get_ctrl(&ctx->cxl);

		ctrl->work_type = event.work_type;
		ctrl->input_len = event.input_len;
		ctrl->output_len = event.output_len;
		ctrl->l1_ready = 1;
		ctrl->gpu_done = 0;

		if (event.work_type == WORK_TYPE_LDPC) {
			/* Try OpenCL first; fall back to bare-C */
			if (opencl_compute_ldpc(&ctx->ocl, &event) < 0)
				gpu_compute_ldpc(&ctx->cxl, &event);
			ctx->processed_ldpc++;
		} else if (event.work_type == WORK_TYPE_FFT) {
			if (opencl_compute_fft(&ctx->ocl, &event) < 0)
				gpu_compute_fft(&ctx->cxl, &event);
			ctx->processed_fft++;
		}

		clock_gettime(CLOCK_MONOTONIC, &t_end);
		uint64_t lat = timespec_diff_ns(&t_start, &t_end);

		ctx->total_latency_ns += lat;
		signal_completion(&ctx->cxl, &event, lat);

		if (ctx->verbose)
			printf("[gpu_daemon] work_type=%u latency=%lu ns\n",
			       event.work_type, (unsigned long)lat);
	}

	return NULL;
}

static int enqueue_event(gpu_daemon_ctx_t *ctx, const struct offload_event *event)
{
	pthread_mutex_lock(&ctx->queue_mutex);
	if (ctx->queue_count >= MAX_QUEUE_DEPTH) {
		pthread_mutex_unlock(&ctx->queue_mutex);
		return -EBUSY;
	}

	ctx->queue[ctx->queue_tail] = *event;
	ctx->queue_tail = (ctx->queue_tail + 1) % MAX_QUEUE_DEPTH;
	ctx->queue_count++;
	pthread_cond_signal(&ctx->queue_cond);
	pthread_mutex_unlock(&ctx->queue_mutex);
	return 0;
}

static void handle_sigint(int sig)
{
	(void)sig;
	g_ctx.running = 0;
	pthread_cond_broadcast(&g_ctx.queue_cond);
}

static int setup_socket(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

	unlink(path);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };

	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	listen(fd, 8);
	return fd;
}

int main(int argc, char **argv)
{
	const char *cxl_path    = "/dev/dax0.0";
	const char *socket_path = GPU_DAEMON_SOCKET;
	const char *kernel_path = NULL;   /* opencl_kernels.cl */
	const char *emu_log     = NULL;   /* emulation_mode.txt */

	static struct option opts[] = {
		{ "cxl-path",      required_argument, 0, 'c' },
		{ "socket",        required_argument, 0, 's' },
		{ "kernel-path",   required_argument, 0, 'k' },
		{ "emulation-log", required_argument, 0, 'l' },
		{ "verbose",       no_argument,       0, 'v' },
		{ 0, 0, 0, 0 },
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "c:s:k:l:v", opts, NULL)) != -1) {
		switch (opt) {
		case 'c': cxl_path    = optarg; break;
		case 's': socket_path = optarg; break;
		case 'k': kernel_path = optarg; break;
		case 'l': emu_log     = optarg; break;
		case 'v': g_ctx.verbose = 1;    break;
		default:  return 1;
		}
	}

	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);

	if (cxl_init(&g_ctx.cxl, cxl_path) < 0) {
		fprintf(stderr, "cxl_init failed, trying shm fallback\n");
		if (cxl_init_shm_fallback(&g_ctx.cxl) < 0) {
			perror("cxl_init_shm_fallback");
			return 1;
		}
	}

	/* Initialise OpenCL (PoCL CPU backend; GPU if available) */
	if (opencl_init(&g_ctx.ocl, &g_ctx.cxl, kernel_path) < 0) {
		fprintf(stderr, "[daemon] OpenCL unavailable — bare-C fallback\n");
	}
	if (emu_log)
		opencl_log_backend(&g_ctx.ocl, emu_log);

	g_ctx.sock_listen = setup_socket(socket_path);
	if (g_ctx.sock_listen < 0) {
		perror("socket");
		return 1;
	}

	pthread_mutex_init(&g_ctx.queue_mutex, NULL);
	pthread_cond_init(&g_ctx.queue_cond, NULL);
	g_ctx.running = 1;
	pthread_create(&g_ctx.worker_thread, NULL, gpu_worker_thread, &g_ctx);

	printf("[gpu_daemon] listening on %s, CXL mode=%s\n",
	       socket_path, g_ctx.cxl.mode);

	while (g_ctx.running) {
		struct pollfd pfd = { .fd = g_ctx.sock_listen, .events = POLLIN };
		int pr = poll(&pfd, 1, 500);

		if (pr < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (pr == 0)
			continue;

		int client = accept(g_ctx.sock_listen, NULL, NULL);

		if (client < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}

		struct offload_event event;

		ssize_t n = recv(client, &event, sizeof(event), MSG_WAITALL);

		if (n == (ssize_t)sizeof(event))
			enqueue_event(&g_ctx, &event);
		close(client);
	}

	g_ctx.running = 0;
	pthread_cond_broadcast(&g_ctx.queue_cond);
	pthread_join(g_ctx.worker_thread, NULL);
	close(g_ctx.sock_listen);
	unlink(socket_path);
	opencl_fini(&g_ctx.ocl);
	cxl_fini(&g_ctx.cxl);
	return 0;
}
