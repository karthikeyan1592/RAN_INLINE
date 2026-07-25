#ifndef L1_INTERCEPT_H
#define L1_INTERCEPT_H

#include <stdint.h>

#define WORK_TYPE_LDPC  1
#define WORK_TYPE_FFT   2
#define WORK_TYPE_DONE  3

struct offload_event {
	uint32_t work_type;
	uint32_t pid;
	uint64_t input_addr;
	uint64_t output_addr;
	uint32_t input_len;
	uint32_t output_len;
	uint64_t timestamp_ns;
};

struct completion_info {
	uint32_t pid;
	uint32_t work_type;
	uint64_t result_addr;
	uint64_t latency_ns;
	int      retcode;
};

struct stats {
	uint64_t ldpc_offloads;
	uint64_t fft_offloads;
	uint64_t total_bytes;
	uint64_t total_latency_ns;
	uint64_t errors;
};

#endif /* L1_INTERCEPT_H */
