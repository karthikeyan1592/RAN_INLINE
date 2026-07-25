// SPDX-License-Identifier: GPL-2.0
// Phase 3 Gate 3 (b) — XDP packet timestamper on veth-gnb.
//
// Attaches to the gnb-ns side of the rfsimulator veth link.
// Timestamps every packet and pushes (timestamp_ns, len) into a
// BPF_MAP_TYPE_RINGBUF.  Never drops or modifies packets (XDP_PASS).
//
// This is INDEPENDENT of the uprobe interception (ldpc_probe.bpf.c).
// Two separate eBPF programs, two separate evidence streams.
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>

struct pkt_event {
    __u64 timestamp_ns;
    __u32 pkt_len;
    __u32 pad;
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);  /* 1 MiB ring */
} rfsim_pkts SEC(".maps");

SEC("xdp")
int observe_rfsim(struct xdp_md *ctx)
{
    struct pkt_event *e = bpf_ringbuf_reserve(&rfsim_pkts, sizeof(*e), 0);
    if (e) {
        e->timestamp_ns = bpf_ktime_get_ns();
        e->pkt_len      = ctx->data_end - ctx->data;
        e->pad          = 0;
        bpf_ringbuf_submit(e, 0);
    }
    return XDP_PASS;
}

char LICENSE[] SEC("license") = "GPL";
