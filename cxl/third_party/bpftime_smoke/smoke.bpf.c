// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// bpftime userspace uprobe smoke probe for Gate 0.1.
// Attaches to test_target:target_func, counts invocations in a map.
// Representative of the real Phase-2 probe (reads ctx, writes a small
// record) but kept cheap so the measured overhead is the attach cost.
#define BPF_NO_GLOBAL_DATA
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} call_count SEC(".maps");

SEC("uprobe//root/linux_env/cxl/third_party/bpftime_smoke/test_target:target_func")
int BPF_UPROBE(on_target_func, int x)
{
    __u32 k = 0;
    __u64 *v = bpf_map_lookup_elem(&call_count, &k);
    if (v) *v += 1;
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
