#define _GNU_SOURCE
#include "fft.h"
#include "ldpc.h"

#include <complex.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_OFDM_SYMBOLS 14
#define NUM_CODE_BLOCKS  4
#define FFT_SIZE         4096
#define SLOT_BUDGET_US   500

static uint64_t timespec_diff_us(const struct timespec *a, const struct timespec *b)
{
	int64_t sec = (int64_t)(b->tv_sec - a->tv_sec);
	int64_t nsec = (int64_t)(b->tv_nsec - a->tv_nsec);
	int64_t us = sec * 1000000LL + nsec / 1000LL;

	return us > 0 ? (uint64_t)us : 0;
}

static void generate_rx_samples(float complex *buf, int slot)
{
	for (int i = 0; i < NUM_OFDM_SYMBOLS; i++)
		fft_gen_ofdm_signal(buf + i * FFT_SIZE, FFT_SIZE, 106);
	(void)slot;
}

static void channel_estimate_and_equalize(float complex *rx, float complex *eq)
{
	memcpy(eq, rx, NUM_OFDM_SYMBOLS * FFT_SIZE * sizeof(float complex));
}

void run_l1_simulation(int num_slots, int use_offload)
{
	float complex *rx_buf = calloc(NUM_OFDM_SYMBOLS * FFT_SIZE, sizeof(float complex));
	float complex *eq_buf = calloc(NUM_OFDM_SYMBOLS * FFT_SIZE, sizeof(float complex));
	float complex *fft_in = calloc(FFT_SIZE, sizeof(float complex));
	float complex *fft_out = calloc(FFT_SIZE, sizeof(float complex));
	uint8_t *ldpc_in = calloc(8448, 1);
	uint8_t *ldpc_out = calloc(8448, 1);

	if (!rx_buf || !eq_buf || !fft_in || !fft_out || !ldpc_in || !ldpc_out) {
		fprintf(stderr, "allocation failed\n");
		return;
	}

	if (use_offload)
		setenv("RAN_OFFLOAD", "1", 1);
	else
		unsetenv("RAN_OFFLOAD");

	for (int slot = 0; slot < num_slots; slot++) {
		struct timespec t_start, t_end;

		clock_gettime(CLOCK_MONOTONIC, &t_start);

		generate_rx_samples(rx_buf, slot);

		for (int sym = 0; sym < NUM_OFDM_SYMBOLS; sym++) {
			memcpy(fft_in, rx_buf + sym * FFT_SIZE,
			       FFT_SIZE * sizeof(float complex));
			fft_params_t fp = {
				.input = fft_in,
				.output = fft_out,
				.N = FFT_SIZE,
				.direction = -1,
				.normalized = 1,
			};
			fft_process(&fp);
		}

		channel_estimate_and_equalize(rx_buf, eq_buf);

		for (int cb = 0; cb < NUM_CODE_BLOCKS; cb++) {
			ldpc_gen_test_input(ldpc_in, 8448, 12.0f);
			ldpc_params_t lp = {
				.input = ldpc_in,
				.output = ldpc_out,
				.input_len = 8448,
				.output_len = 8448,
				.base_graph = 1,
				.lifting_size = 384,
				.code_rate = 0.5f,
			};
			ldpc_decode(&lp);
		}

		clock_gettime(CLOCK_MONOTONIC, &t_end);
		uint64_t latency_us = timespec_diff_us(&t_start, &t_end);

		if (slot < 5 || slot == num_slots - 1)
			printf("slot=%d latency=%lu us offload=%d miss=%s\n",
			       slot, (unsigned long)latency_us, use_offload,
			       latency_us > SLOT_BUDGET_US ? "yes" : "no");
	}

	free(rx_buf);
	free(eq_buf);
	free(fft_in);
	free(fft_out);
	free(ldpc_in);
	free(ldpc_out);
}

extern void ldpc_set_iterations(int iters);

int main(int argc, char **argv)
{
	int num_slots = 100;
	int use_offload = 0;
	int calibration = 0;

	static struct option opts[] = {
		{ "slots", required_argument, 0, 'n' },
		{ "offload", no_argument, 0, 'o' },
		{ "calibration", no_argument, 0, 'c' },
		{ 0, 0, 0, 0 },
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "n:oc", opts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			num_slots = atoi(optarg);
			break;
		case 'o':
			use_offload = 1;
			break;
		case 'c':
			calibration = 1;
			break;
		default:
			return 1;
		}
	}

	ldpc_init();
	fft_init();

	if (calibration) {
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
		printf("calibration_ldpc_us=%lu\n",
		       (unsigned long)timespec_diff_us(&t0, &t1));
		free(in);
		free(out);
	} else {
		run_l1_simulation(num_slots, use_offload);
	}

	ldpc_cleanup();
	fft_cleanup();
	return 0;
}
