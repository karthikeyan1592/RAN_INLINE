# Gate 2 — CXL NUMA + BPF Trigger
Date: 2026-07-01 (v8 run with CC-004 BG fix)

## CXL region setup (from phase 4 output)

```
[v8] CXL NUMA node 1: 1920 MB
[v8] DBG: shm_unlink
[v8] DBG: shm_open
[v8] DBG: ftruncate fd=4
[v8] DBG: mmap
[v8] DBG: mbind strict
[v8] DBG: mbind done
[v8] CXL region mapped: base=0x7f2510000000 size=64MB node=1 cxl_ok=YES
```

## BPF load + config_map initialization

```
[v8] BPF object loaded
[v8] config_map initialized (cxl_base=0 sentinel — awaiting bench VA)
[v8] bpftime_uprobe_registered (pre-fork, pid=-1, offset=0x3fc80)
```

## Gate 2 self-verdict: PASS

- CXL mapped at 0x7f2510000000, node=1 (CXL=YES)
- shm_open("/cxl_region_v8") + mbind(MPOL_BIND, node=1) confirmed
- BPF object loaded via bpftime syscall-server (libbpftime-syscall-server.so)
- config_map sentinel set; uprobe registered pre-fork for benchmark binary
- DEV-033: numactl --membind=1 direct SIGILL; llr_consumer_v8 calls mbind() directly (works)
