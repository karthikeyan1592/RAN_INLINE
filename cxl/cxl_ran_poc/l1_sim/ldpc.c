#define _GNU_SOURCE
#include "ldpc.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LDPC_MAX_ITER_DEFAULT 20
#define LDPC_MAX_ITER_LARGE   50

extern int l1_offload_try(uint32_t work_type, const void *params)
	__attribute__((weak));

static int g_max_iterations = LDPC_MAX_ITER_DEFAULT;
static int g_ldpc_ready;

static int8_t *g_llr_buf;
static int8_t *g_msg_buf;
static size_t g_buf_capacity;

static void ldpc_min_sum_block(const int8_t *llr, uint8_t *out,
			       size_t num_bits, int iterations)
{
	size_t n = num_bits;
	if (n == 0)
		return;

	for (size_t i = 0; i < n; i++)
		out[i] = llr[i] < 0 ? 1 : 0;

	for (int it = 0; it < iterations; it++) {
		for (size_t i = 0; i < n; i++) {
			int8_t v = llr[i];
			int8_t acc = v;

			for (size_t j = 1; j < 32; j++) {
				size_t k = (i * 9973 + j * 4099) % n;
				int8_t m = llr[k];

				acc = (int8_t)(((acc ^ m) >= 0) ?
						(abs(acc) < abs(m) ? acc : m) :
						((acc > 0) ? -abs(m) : abs(m)));
			}
			g_msg_buf[i] = acc;
		}

		for (size_t i = 0; i < n; i++) {
			int8_t total = llr[i];

			for (size_t j = 0; j < 24; j++) {
				size_t k = (i * 6151 + j * 3571) % n;
				total = (int8_t)(total + g_msg_buf[k] / 4);
			}
			g_llr_buf[i] = total;
			out[i] = total < 0 ? 1 : 0;
		}

		memcpy((void *)llr, g_llr_buf, n);
	}
}

int ldpc_init(void)
{
	if (g_ldpc_ready)
		return 0;

	g_buf_capacity = LDPC_MAX_CB_SIZE;
	g_llr_buf = calloc(g_buf_capacity, sizeof(int8_t));
	g_msg_buf = calloc(g_buf_capacity, sizeof(int8_t));
	if (!g_llr_buf || !g_msg_buf)
		return -1;

	g_ldpc_ready = 1;
	return 0;
}

void ldpc_cleanup(void)
{
	free(g_llr_buf);
	free(g_msg_buf);
	g_llr_buf = NULL;
	g_msg_buf = NULL;
	g_ldpc_ready = 0;
}

void ldpc_gen_test_input(uint8_t *buf, size_t len, float snr_db)
{
	static unsigned seed = 42;
	float noise = powf(10.0f, -snr_db / 20.0f);

	for (size_t i = 0; i < len; i++) {
		int bit = (seed = seed * 1103515245 + 12345) & 1;
		float llr = bit ? 8.0f : -8.0f;

		llr += ((seed >> 8) & 0xff) / 255.0f * noise * 4.0f - noise * 2.0f;
		buf[i] = (uint8_t)(int8_t)llr;
	}
}

int ldpc_encode(const ldpc_params_t *params)
{
	if (!params || !params->input || !params->output)
		return -1;

	memcpy(params->output, params->input,
	       params->output_len < params->input_len ?
		       params->output_len : params->input_len);
	return 0;
}

int ldpc_decode_internal(const ldpc_params_t *params)
{
	if (!params || !params->input || !params->output || !g_ldpc_ready)
		return -1;

	size_t nbits = params->input_len;
	if (nbits > g_buf_capacity)
		nbits = g_buf_capacity;

	for (size_t i = 0; i < nbits; i++)
		g_llr_buf[i] = (int8_t)params->input[i];

	int iters = g_max_iterations;
	if (params->lifting_size >= 384 && nbits > 4096)
		iters = LDPC_MAX_ITER_LARGE;

	ldpc_min_sum_block(g_llr_buf, params->output, nbits, iters);
	return 0;
}

int ldpc_decode(const ldpc_params_t *params)
{
	if (l1_offload_try &&
	    l1_offload_try(1, params) == 0)
		return 0;

	return ldpc_decode_internal(params);
}

int ldpc_verify(const uint8_t *encoded, const uint8_t *decoded, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if ((encoded[i] & 1) != (decoded[i] & 1))
			return -1;
	}
	return 0;
}

void ldpc_set_iterations(int iters)
{
	if (iters > 0)
		g_max_iterations = iters;
}

int ldpc_get_iterations(void)
{
	return g_max_iterations;
}
