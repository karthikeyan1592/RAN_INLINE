#ifndef FFT_H
#define FFT_H

#include <complex.h>
#include <stddef.h>

typedef struct {
	float complex *input;
	float complex *output;
	size_t         N;
	int            direction;
	int            normalized;
} fft_params_t;

int fft_init(void);
int fft_process(const fft_params_t *params);
int fft_process_internal(const fft_params_t *params);
void fft_cleanup(void);
void fft_gen_ofdm_signal(float complex *buf, size_t N, int num_subcarriers);

#endif /* FFT_H */
