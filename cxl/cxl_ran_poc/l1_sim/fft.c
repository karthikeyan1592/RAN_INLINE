#define _GNU_SOURCE
#include "fft.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int l1_offload_try(uint32_t work_type, const void *params)
	__attribute__((weak));

static int g_fft_ready;
static float complex *g_twiddle;
static size_t g_twiddle_n;

static int fft_bit_reverse(size_t i, size_t bits)
{
	size_t r = 0;

	for (size_t b = 0; b < bits; b++)
		r = (r << 1) | ((i >> b) & 1);
	return r;
}

static int fft_ensure_twiddle(size_t N)
{
	if (g_twiddle && g_twiddle_n == N)
		return 0;

	free(g_twiddle);
	g_twiddle = malloc(N * sizeof(float complex));
	if (!g_twiddle)
		return -1;

	for (size_t k = 0; k < N; k++)
		g_twiddle[k] = cexpf(-2.0f * (float)M_PI * k / (float)N);

	g_twiddle_n = N;
	return 0;
}

int fft_init(void)
{
	g_fft_ready = 1;
	return 0;
}

void fft_cleanup(void)
{
	free(g_twiddle);
	g_twiddle = NULL;
	g_twiddle_n = 0;
	g_fft_ready = 0;
}

void fft_gen_ofdm_signal(float complex *buf, size_t N, int num_subcarriers)
{
	for (size_t i = 0; i < N; i++)
		buf[i] = 0.0f + 0.0f * I;

	int half = num_subcarriers / 2;
	for (int sc = -half; sc < half; sc++) {
		size_t idx = (size_t)((sc + (int)N) % (int)N);
		float phase = 2.0f * (float)M_PI * sc / (float)N;

		buf[idx] = (cosf(phase) + sinf(phase) * I) * 0.5f;
	}
}

int fft_process_internal(const fft_params_t *params)
{
	if (!params || !params->input || !params->output || !g_fft_ready)
		return -1;

	size_t N = params->N;
	if (N < 2 || (N & (N - 1)) != 0)
		return -1;

	if (fft_ensure_twiddle(N) < 0)
		return -1;

	size_t bits = 0;
	for (size_t t = N; t > 1; t >>= 1)
		bits++;

	for (size_t i = 0; i < N; i++)
		params->output[fft_bit_reverse(i, bits)] = params->input[i];

	for (size_t len = 2; len <= N; len <<= 1) {
		size_t half = len / 2;
		size_t step = N / len;

		for (size_t i = 0; i < N; i += len) {
			for (size_t j = 0; j < half; j++) {
				float complex w = g_twiddle[j * step];
				float complex u = params->output[i + j];
				float complex v = params->output[i + j + half] * w;

				params->output[i + j] = u + v;
				params->output[i + j + half] = u - v;
			}
		}
	}

	if (params->direction < 0) {
		for (size_t i = 0; i < N; i++)
			params->output[i] = conjf(params->output[i]);
	}

	if (params->normalized) {
		for (size_t i = 0; i < N; i++)
			params->output[i] /= (float)N;
	}

	return 0;
}

int fft_process(const fft_params_t *params)
{
	if (l1_offload_try &&
	    l1_offload_try(2, params) == 0)
		return 0;

	return fft_process_internal(params);
}
