#ifndef LDPC_H
#define LDPC_H

#include <stdint.h>
#include <stddef.h>

#define LDPC_MAX_CB_SIZE    8448
#define LDPC_MAX_MSG_SIZE   8192
#define LDPC_LIFTING_SIZE   384

typedef struct {
	uint8_t *input;
	uint8_t *output;
	size_t   input_len;
	size_t   output_len;
	int      base_graph;
	int      lifting_size;
	float    code_rate;
} ldpc_params_t;

int ldpc_init(void);
int ldpc_encode(const ldpc_params_t *params);
int ldpc_decode(const ldpc_params_t *params);
int ldpc_decode_internal(const ldpc_params_t *params);
void ldpc_gen_test_input(uint8_t *buf, size_t len, float snr_db);
int ldpc_verify(const uint8_t *encoded, const uint8_t *decoded, size_t len);
void ldpc_cleanup(void);
void ldpc_set_iterations(int iters);
int ldpc_get_iterations(void);

#endif /* LDPC_H */
