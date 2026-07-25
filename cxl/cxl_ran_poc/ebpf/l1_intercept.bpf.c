// SPDX-License-Identifier: GPL-2.0
// CO-RE BPF program: include vmlinux.h first for kernel types,
// then libbpf helpers. Do NOT include <linux/bpf.h> — vmlinux.h
// already provides all necessary kernel definitions.
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_types.h"

#define WORK_TYPE_LDPC  1
#define WORK_TYPE_FFT   2

struct offload_event {
	__u32 work_type;
	__u32 pid;
	__u64 input_addr;
	__u64 output_addr;
	__u32 input_len;
	__u32 output_len;
	__u64 timestamp_ns;
};

struct stats {
	__u64 ldpc_offloads;
	__u64 fft_offloads;
	__u64 total_bytes;
	__u64 total_latency_ns;
	__u64 errors;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} offload_ringbuf SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u32);
	__type(value, __u64);
} completion_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct stats);
} stats_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} offload_enabled SEC(".maps");

SEC("uprobe/ldpc_decode")
int intercept_ldpc_decode(struct pt_regs *ctx)
{
	const void *params = (const void *)PT_REGS_PARM1(ctx);
	__u32 key = 0;
	__u32 *enabled = bpf_map_lookup_elem(&offload_enabled, &key);

	if (!enabled || !*enabled)
		return 0;

	struct offload_event *event = bpf_ringbuf_reserve(&offload_ringbuf,
							  sizeof(*event), 0);

	if (!event)
		return 0;

	event->work_type = WORK_TYPE_LDPC;
	event->pid = bpf_get_current_pid_tgid() >> 32;
	event->timestamp_ns = bpf_ktime_get_ns();

	bpf_probe_read_user(&event->input_addr, sizeof(event->input_addr), params);
	bpf_probe_read_user(&event->output_addr, sizeof(event->output_addr),
			    (const char *)params + 8);
	bpf_probe_read_user(&event->input_len, sizeof(event->input_len),
			    (const char *)params + 16);

	/*
	 * Update stats while `event` is still valid (before ringbuf submit).
	 * bpf_ringbuf_submit() releases the slot, making the pointer a
	 * dangling reference that the verifier rejects on any subsequent read.
	 */
	struct stats *s = bpf_map_lookup_elem(&stats_map, &key);

	if (s) {
		__sync_fetch_and_add(&s->ldpc_offloads, 1);
		__sync_fetch_and_add(&s->total_bytes, event->input_len);
	}

	bpf_ringbuf_submit(event, 0);

	return 0;
}

SEC("uprobe/fft_process")
int intercept_fft_process(struct pt_regs *ctx)
{
	const void *params = (const void *)PT_REGS_PARM1(ctx);
	__u32 key = 0;
	__u32 *enabled = bpf_map_lookup_elem(&offload_enabled, &key);

	if (!enabled || !*enabled)
		return 0;

	struct offload_event *event = bpf_ringbuf_reserve(&offload_ringbuf,
							  sizeof(*event), 0);

	if (!event)
		return 0;

	event->work_type = WORK_TYPE_FFT;
	event->pid = bpf_get_current_pid_tgid() >> 32;
	event->timestamp_ns = bpf_ktime_get_ns();

	bpf_probe_read_user(&event->input_addr, sizeof(event->input_addr), params);

	__u64 N = 0;

	bpf_probe_read_user(&N, sizeof(N), (const char *)params + 16);
	event->input_len = (__u32)(N * 8);

	bpf_ringbuf_submit(event, 0);

	struct stats *s = bpf_map_lookup_elem(&stats_map, &key);

	if (s)
		__sync_fetch_and_add(&s->fft_offloads, 1);

	return 0;
}

char LICENSE[] SEC("license") = "GPL";
