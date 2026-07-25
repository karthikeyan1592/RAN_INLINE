/* nic_timeline_consumer.c — Phase 3 Gate 3 (b)
 *
 * Loads xdp_rfsim_observe.bpf.o via libbpf, attaches it to veth-gnb
 * (gnb-ns side), polls the BPF_MAP_TYPE_RINGBUF, and writes:
 *   paper/results/nic_packet_timeline.csv
 *   columns: timestamp_ns, packet_len, direction
 *
 * On SIGINT: also prints inter-arrival time statistics.
 *
 * Usage (must run as root, with gnb-ns veth-gnb visible):
 *   ./nic_timeline_consumer <ifname> <out_csv> [max_pkts]
 *   e.g. ./nic_timeline_consumer veth-gnb /root/.../nic_packet_timeline.csv 50000
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <net/if.h>
#include <unistd.h>

#include <linux/if_link.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

struct pkt_event {
    uint64_t timestamp_ns;
    uint32_t pkt_len;
    uint32_t pad;
};

/* ring-buffer callback state */
struct cb_ctx {
    FILE    *csv;
    int      n;
    int      max;
    uint64_t last_ts;
    /* running Welford for inter-arrival stddev */
    double   mean_us;
    double   M2_us;
    int      n_ia;
    uint64_t min_ia_ns;
    uint64_t max_ia_ns;
};

static int pkt_cb(void *ctx_v, void *data, size_t sz)
{
    struct cb_ctx *ctx = ctx_v;
    if (sz < sizeof(struct pkt_event)) return 0;
    struct pkt_event *e = data;

    /* direction: "in" — we see all packets on the RFsim link */
    fprintf(ctx->csv, "%llu,%u,in\n",
            (unsigned long long)e->timestamp_ns, e->pkt_len);

    /* inter-arrival statistics (Welford online) */
    if (ctx->last_ts != 0 && e->timestamp_ns > ctx->last_ts) {
        uint64_t ia_ns = e->timestamp_ns - ctx->last_ts;
        double ia_us = ia_ns / 1000.0;
        ctx->n_ia++;
        double delta = ia_us - ctx->mean_us;
        ctx->mean_us += delta / ctx->n_ia;
        ctx->M2_us   += delta * (ia_us - ctx->mean_us);
        if (ia_ns < ctx->min_ia_ns || ctx->min_ia_ns == 0) ctx->min_ia_ns = ia_ns;
        if (ia_ns > ctx->max_ia_ns) ctx->max_ia_ns = ia_ns;
    }
    ctx->last_ts = e->timestamp_ns;
    ctx->n++;

    if (ctx->n % 1000 == 0)
        fprintf(stderr, "\r[nic] %d packets...", ctx->n);

    if (ctx->n >= ctx->max) {
        g_stop = 1;
        return -1;  /* stop ring-buffer polling */
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ifname> <out_csv> [max_pkts]\n", argv[0]);
        return 1;
    }
    const char *ifname   = argv[1];
    const char *csv_path = argv[2];
    int max_pkts = (argc > 3) ? atoi(argv[3]) : 50000;

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    /* ---- load BPF object ---- */
    libbpf_set_print(NULL);
    struct bpf_object *obj = bpf_object__open("xdp_rfsim_observe.bpf.o");
    if (!obj) { fprintf(stderr, "bpf_object__open failed\n"); return 1; }
    if (bpf_object__load(obj)) { fprintf(stderr, "bpf_object__load failed\n"); return 1; }

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, "observe_rfsim");
    if (!prog) { fprintf(stderr, "program not found\n"); return 1; }
    int prog_fd = bpf_program__fd(prog);

    /* ---- attach XDP to ifname (in current netns, so run inside gnb-ns or root ns) ---- */
    unsigned int ifindex = if_nametoindex(ifname);
    if (!ifindex) { perror("if_nametoindex"); return 1; }

    /* XDP_FLAGS_SKB_MODE (generic XDP) — veth doesn't support native XDP in all kernels */
    if (bpf_xdp_attach(ifindex, prog_fd, XDP_FLAGS_SKB_MODE, NULL)) {
        perror("bpf_xdp_attach");
        return 1;
    }
    fprintf(stderr, "[nic] XDP attached to %s (ifindex=%u)\n", ifname, ifindex);

    /* ---- open ring buffer ---- */
    struct bpf_map *rb_map = bpf_object__find_map_by_name(obj, "rfsim_pkts");
    if (!rb_map) { fprintf(stderr, "map rfsim_pkts not found\n"); return 1; }

    FILE *csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); return 1; }
    fprintf(csv, "timestamp_ns,packet_len,direction\n");

    struct cb_ctx ctx = {
        .csv = csv, .n = 0, .max = max_pkts,
        .last_ts = 0, .mean_us = 0, .M2_us = 0, .n_ia = 0,
        .min_ia_ns = 0, .max_ia_ns = 0,
    };

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(rb_map), pkt_cb, &ctx, NULL);
    if (!rb) { fprintf(stderr, "ring_buffer__new failed\n"); return 1; }

    fprintf(stderr, "[nic] collecting %d packets into %s ...\n", max_pkts, csv_path);
    while (!g_stop) {
        ring_buffer__poll(rb, 100 /* ms */);
    }

    fclose(csv);
    ring_buffer__free(rb);

    /* ---- detach XDP ---- */
    bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE, NULL);
    fprintf(stderr, "\n[nic] XDP detached from %s\n", ifname);

    /* ---- inter-arrival statistics ---- */
    double stddev_us = (ctx.n_ia > 1) ? sqrt(ctx.M2_us / (ctx.n_ia - 1)) : 0.0;
    fprintf(stdout,
            "\n[nic] PACKET TIMELINE STATISTICS:\n"
            "  total_packets:      %d\n"
            "  n_inter_arrivals:   %d\n"
            "  mean_ia_us:         %.3f\n"
            "  stddev_ia_us:       %.3f\n"
            "  min_ia_us:          %.3f\n"
            "  max_ia_us:          %.3f\n"
            "  slot_period_us:     500 (expected, mu=1)\n",
            ctx.n, ctx.n_ia,
            ctx.mean_us,
            stddev_us,
            ctx.min_ia_ns / 1000.0,
            ctx.max_ia_ns / 1000.0);
    fflush(stdout);

    bpf_object__close(obj);
    return 0;
}
