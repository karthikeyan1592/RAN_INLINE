// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Loader for the Gate 0.1 bpftime smoke probe.
// Run under LD_PRELOAD=libbpftime-syscall-server.so so the program is
// registered into bpftime's shared memory; the victim (test_target)
// run under LD_PRELOAD=libbpftime-agent.so then executes the uprobe
// in userspace. On SIGINT/SIGTERM we dump the call counter.
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "smoke.skel.h"

static volatile int exiting = 0;
static void on_sig(int s) { exiting = 1; }

static int pr(enum libbpf_print_level l, const char *f, va_list a) {
    return vfprintf(stderr, f, a);
}

int main(void) {
    libbpf_set_print(pr);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    struct smoke_bpf *skel = smoke_bpf__open();
    if (!skel) { fprintf(stderr, "open failed\n"); return 1; }
    if (smoke_bpf__load(skel)) { fprintf(stderr, "load failed\n"); return 1; }
    if (smoke_bpf__attach(skel)) { fprintf(stderr, "attach failed\n"); return 1; }

    fprintf(stderr, "smoke loader: probe attached, waiting (Ctrl-C to dump)\n");
    while (!exiting) sleep(1);

    __u32 k = 0; __u64 v = 0;
    int mfd = bpf_map__fd(skel->maps.call_count);
    if (bpf_map_lookup_elem(mfd, &k, &v) == 0)
        fprintf(stderr, "call_count = %llu\n", (unsigned long long)v);
    else
        fprintf(stderr, "map lookup failed\n");
    smoke_bpf__destroy(skel);
    return 0;
}
