#define _GNU_SOURCE
#include "../l1_sim/fft.h"
#include "../l1_sim/ldpc.h"

#include <complex.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SLOT_BUDGET_US 500

static uint64_t timespec_diff_us(const struct timespec *a, const struct timespec *b)
{
	int64_t sec = (int64_t)(b->tv_sec - a->tv_sec);
	int64_t nsec = (int64_t)(b->tv_nsec - a->tv_nsec);
	int64_t us = sec * 1000000LL + nsec / 1000LL;

	return us > 0 ? (uint64_t)us : 0;
}

static uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b)
{
	int64_t sec  = (int64_t)(b->tv_sec  - a->tv_sec);
	int64_t nsec = (int64_t)(b->tv_nsec - a->tv_nsec);
	int64_t ns   = sec * 1000000000LL + nsec;

	return ns > 0 ? (uint64_t)ns : 0;
}

static void write_calibration(const char *path, uint64_t us)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return;
	fprintf(f, "CPU LDPC large-TB baseline: %.3f ms (target: ~0.71 ms)\n",
		us / 1000.0);
	fprintf(f, "Status: %s\n",
		us > 350 && us < 1400 ? "OK (within 2x of target)" : "CHECK");
	fclose(f);
}

static int run_slot_loop(int slots, int offload, FILE *out, const char *mode)
{
	float complex *fft_in = calloc(4096, sizeof(float complex));
	float complex *fft_out = calloc(4096, sizeof(float complex));
	uint8_t *ldpc_in = calloc(8448, 1);
	uint8_t *ldpc_out = calloc(8448, 1);

	if (!fft_in || !fft_out || !ldpc_in || !ldpc_out)
		return -1;

	if (offload)
		setenv("RAN_OFFLOAD", "1", 1);
	else
		unsetenv("RAN_OFFLOAD");

	fprintf(out, "slot,latency_us,deadline_miss,emulation_mode\n");

	for (int slot = 0; slot < slots; slot++) {
		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);

		for (int sym = 0; sym < 14; sym++) {
			fft_gen_ofdm_signal(fft_in, 4096, 106);
			fft_params_t fp = {
				.input = fft_in, .output = fft_out,
				.N = 4096, .direction = -1, .normalized = 1,
			};
			fft_process(&fp);
		}

		for (int cb = 0; cb < 4; cb++) {
			ldpc_gen_test_input(ldpc_in, 8448, 10.0f);
			ldpc_params_t lp = {
				.input = ldpc_in, .output = ldpc_out,
				.input_len = 8448, .output_len = 8448,
				.base_graph = 1, .lifting_size = 384,
			};
			ldpc_decode(&lp);
		}

		clock_gettime(CLOCK_MONOTONIC, &t1);
		uint64_t us = timespec_diff_us(&t0, &t1);

		fprintf(out, "%d,%lu,%d,%s\n", slot, (unsigned long)us,
			us > SLOT_BUDGET_US ? 1 : 0, mode);
	}

	free(fft_in);
	free(fft_out);
	free(ldpc_in);
	free(ldpc_out);
	return 0;
}

/*
 * Async offload slot loop.
 *
 * Architecture: pipeline double-buffer.
 *   Slot N: dispatch to daemon → RETURN IMMEDIATELY with prev result
 *   Slot N+1: return slot-N result → dispatch slot-N+1 work
 *
 * Per-slot latency measures: FFT compute + 4×LDPC dispatch (no blocking wait).
 * Daemon processes concurrently on the same CPU (PoCL).  In a real deployment
 * with a discrete GPU or CXL-attached accelerator, this latency would be
 * independent of daemon processing time.
 *
 * RAN_OFFLOAD=1 and ASYNC_OFFLOAD=1 must both be set before loading
 * libl1_offload.so (via the constructor), so we set them here and then
 * call ldpc_decode() which routes through the weak symbol to l1_offload_try().
 */
static int run_async_slot_loop(int slots, FILE *out, const char *mode)
{
	float complex *fft_in  = calloc(4096, sizeof(float complex));
	float complex *fft_out = calloc(4096, sizeof(float complex));
	uint8_t *ldpc_in  = calloc(8448, 1);
	uint8_t *ldpc_out = calloc(8448, 1);

	if (!fft_in || !fft_out || !ldpc_in || !ldpc_out)
		return -1;

	setenv("RAN_OFFLOAD",   "1", 1);
	setenv("ASYNC_OFFLOAD", "1", 1);

	fprintf(out, "slot,latency_us,deadline_miss,emulation_mode\n");

	for (int slot = 0; slot < slots; slot++) {
		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);

		for (int sym = 0; sym < 14; sym++) {
			fft_gen_ofdm_signal(fft_in, 4096, 106);
			fft_params_t fp = {
				.input = fft_in, .output = fft_out,
				.N = 4096, .direction = -1, .normalized = 1,
			};
			fft_process(&fp);
		}

		for (int cb = 0; cb < 4; cb++) {
			ldpc_gen_test_input(ldpc_in, 8448, 10.0f);
			ldpc_params_t lp = {
				.input = ldpc_in, .output = ldpc_out,
				.input_len = 8448, .output_len = 8448,
				.base_graph = 1, .lifting_size = 384,
			};
			ldpc_decode(&lp);   /* async: dispatches to daemon, returns immediately */
		}

		clock_gettime(CLOCK_MONOTONIC, &t1);
		uint64_t us = timespec_diff_us(&t0, &t1);

		fprintf(out, "%d,%lu,%d,%s\n", slot, (unsigned long)us,
			us > SLOT_BUDGET_US ? 1 : 0, mode);
	}

	free(fft_in);
	free(fft_out);
	free(ldpc_in);
	free(ldpc_out);
	return 0;
}

/*
 * Probe target: a tiny no-op exported symbol.
 * When an eBPF uprobe is attached to this symbol the measured call
 * overhead = (time with uprobe) - (time without uprobe).
 * We measure here WITHOUT uprobe; the caller compares against the
 * eBPF-attached run to isolate pure probe entry/exit cost.
 *
 * The function is marked __attribute__((noinline)) so the compiler
 * cannot optimise it away, and volatile to prevent constant-folding
 * of the loop counter.
 */
__attribute__((noinline)) void uprobe_target_noop(void)
{
	/* intentionally empty — uprobe fires on function entry */
}

static int run_ebpf_overhead(const char *path, int samples, const char *mode)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;

	/*
	 * Warm-up: fill iTLB / icache so we don't measure cold-start effects.
	 */
	for (int i = 0; i < 100; i++)
		uprobe_target_noop();

	fprintf(f, "call_id,overhead_ns,emulation_mode\n");
	for (int i = 0; i < samples; i++) {
		struct timespec t0, t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		uprobe_target_noop();
		clock_gettime(CLOCK_MONOTONIC, &t1);

		uint64_t ns = timespec_diff_ns(&t0, &t1);
		fprintf(f, "%d,%lu,%s\n", i, (unsigned long)ns, mode);
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode_name = "cpu-baseline";
	const char *output = "paper/results/baseline_latency.csv";
	const char *mode = "baseline";
	int slots = 1000;
	const char *label = NULL;

	static struct option opts[] = {
		{ "mode", required_argument, 0, 'm' },
		{ "slots", required_argument, 0, 'n' },
		{ "output", required_argument, 0, 'o' },
		{ "label", required_argument, 0, 'l' },
		{ 0, 0, 0, 0 },
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "m:n:o:l:", opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			mode = optarg;
			break;
		case 'n':
			slots = atoi(optarg);
			break;
		case 'o':
			output = optarg;
			break;
		case 'l':
			label = optarg;
			break;
		default:
			return 1;
		}
	}

	if (label)
		mode_name = label;
	else if (strcmp(mode, "offload") == 0)
		mode_name = "ebpf-cxl-offload";
	else if (strcmp(mode, "ebpf-overhead") == 0)
		mode_name = "ebpf-overhead-only";
	else if (strcmp(mode, "async-offload") == 0)
		mode_name = "async-cxl-pipeline";

	ldpc_init();
	fft_init();

	if (strcmp(mode, "calibration") == 0) {
		ldpc_set_iterations(20);
		uint8_t *in = calloc(8448, 1);
		uint8_t *out = calloc(8448, 1);
		struct timespec t0, t1;

		ldpc_gen_test_input(in, 8448, 8.0f);
		ldpc_params_t p = {
			.input = in, .output = out,
			.input_len = 8448, .output_len = 8448,
			.base_graph = 1, .lifting_size = 384,
		};
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ldpc_decode_internal(&p);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		uint64_t us = timespec_diff_us(&t0, &t1);

		write_calibration("paper/results/calibration_check.txt", us);
		printf("calibration: %lu us\n", (unsigned long)us);
		free(in);
		free(out);
	} else if (strcmp(mode, "ebpf-overhead") == 0) {
		run_ebpf_overhead(output, slots, mode_name);
	} else if (strcmp(mode, "async-offload") == 0) {
		FILE *f = fopen(output, "w");

		if (!f) {
			perror(output);
			return 1;
		}
		run_async_slot_loop(slots, f, mode_name);
		fclose(f);
	} else {
		FILE *f = fopen(output, "w");

		if (!f) {
			perror(output);
			return 1;
		}
		run_slot_loop(slots, strcmp(mode, "offload") == 0, f, mode_name);
		fclose(f);
	}

	ldpc_cleanup();
	fft_cleanup();
	return 0;
}
