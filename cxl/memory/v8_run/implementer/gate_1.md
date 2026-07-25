# Gate 1 — uprobe Attach + First Fire
Date: 2026-07-01 (v8 run with CC-004 BG fix)

## Verbatim terminal output

```
── 06:47:58 [PHASE 1: CXL NUMA topology] ──────────────────────────────────────────
=== numactl --hardware ===
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3
node 0 size: 3915 MB
node 0 free: 495 MB
node 1 cpus:
node 1 size: 1920 MB
node 1 free: 1920 MB
node distances:
node   0   1
  0:  10  20
  1:  20  10

=== daxctl list ===
[
  {
    "chardev":"dax0.0",
    "size":2111832064,
    "target_node":1,
    "align":2097152,
    "mode":"system-ram",
    "online_memblocks":15,
    "total_memblocks":15,
    "movable":true
  }
]

=== numactl --membind=1 test ===
run_e2e_v8.sh: line 73: 18535 Illegal instruction (core dumped) numactl --membind=1 ls /tmp
SIGILL: mbind shim required (DEV-033 expected — llr_consumer_v8 handles this)

=== gate0_option_a (mbind shim test) ===
[gate0] NUMA max_node=1
[gate0] NUMA node 1 size=1920 MB
[gate0] allocating 1048576 bytes on NUMA node 1...
[gate0] PROOF1 ptr=0x7209834b7000 numa_node=1 cxl_node=YES exit=0
[gate0] wrote sentinel 0xCA7EBEEF at buf[0] (CPU-side, no CL write)
[gate0] OpenCL platform: Portable Computing Language
[gate0] OpenCL device:   pthread-Intel(R) Xeon(R) CPU @ 2.80GHz
[gate0] PROOF2 clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0 (0=OK)
[gate0] sentinel_cpu=0xCA7EBEEF cl_out=0xCA7EBEEF match=YES
[gate0] GATE0 PASS: option=A zero_copy=YES numa_node=1

GATE 1: node_1_mb=1920 PASS
```

## Consumer uprobe registration (from phase 4 output)

```
[v8] bpftime_uprobe_registered (pre-fork, pid=-1, offset=0x3fc80)
[v8] benchmark forked: pid=18560
[v8] waiting for cxl_init.so to map CXL region (up to 5000ms)...
[cxl_init] CXL region mapped at 0x71bd26a00000 (64 MB), VA written to /tmp/cxl_va_v8.bin
[v8] bench_va=0x71bd26a00000 consumer_va=0x7f2510000000 (DEV-040: LLR via scratch_map)
```

## First fire (ring slot 0)

```
[v8] DBG tail=0 ring_slot=0
[v8] DBG ring_ok llr_len=9216 seq=0
[v8] DBG about_scratch slot=0
[v8] DBG scratch_ok buf[0]=-10 scratch_us=24
[v8] DBG pwrite llr_in=0x7f2510000000 cxl_llr_off=0 len=9216
[v8] CXL write: len=9216 us=219871 (QEMU dev-mem path; once only)
```

## Gate 1 self-verdict: PASS

- CXL NUMA node 1: 1920 MB confirmed
- numactl --membind=1 → SIGILL expected (DEV-033; llr_consumer_v8 uses mbind shim directly)
- gate0_option_a: PROOF1 numa_node=1 CXL=YES, PROOF2 zero_copy=YES
- uprobe registered at offset=0x3fc80 for /usr/local/bin/ldpc_decoder_benchmark
- First fire: ring_slot=0, buf[0]=-10 (valid LLR range), scratch_us=24µs
