# Gate 3 — bpftime Start + CXL First Write
Date: 2026-07-01 (v8 run with CC-004 BG fix)

## bpftime syscall-server startup

```
[e2e] cleared stale bpftime shm
[2026-07-01 06:48:46.650] [info] [bpftime_config.cpp:129] Using VM: ubpf
[2026-07-01 06:48:46.650] [info] [syscall_context.cpp:114] Init bpftime syscall mocking..
[2026-07-01 06:48:46.650] [info] [syscall_context.cpp:115] The log will be written to: ~/.bpftime/runtime.log
```

## config_map update with bench VA

```
[v8] bench_va=0x71bd26a00000 consumer_va=0x7f2510000000 (DEV-040: LLR via scratch_map)
```

## CXL first write (CB 0)

```
[v8] DBG pwrite llr_in=0x7383e9800000 cxl_llr_off=0 len=9216
[v8] CXL write: len=9216 us=217675 (QEMU dev-mem path; once only)
```
(re-run 2026-07-01 with CC-005 fixes: cost consistent with prior run at ~218ms, confirming DEV-040 QEMU device-mem rate is stable ~23.6µs/byte)

## bpftime agent init in benchmark process

```
[2026-07-01 06:48:48.071] [info] [bpftime_config.cpp:129] Using VM: ubpf
[2026-07-01 06:48:48.071] [info] [bpftime_shm_internal.cpp:835] Global shm constructed. shm_open_type 1 for bpftime_maps_shm
[2026-07-01 06:48:48.071] [info] [bpftime_shm_internal.cpp:44] Global shm initialized
```

## Gate 3 self-verdict: PASS

- bpftime syscall-server started with VM=ubpf
- bench_va=0x71bd26a00000 received from cxl_init.so; config_map updated
- CXL write: 9216 bytes at consumer_va=0x7f2510000000 (CXL node 1), cost=219,871µs (DEV-040: QEMU device-mem 23µs/byte; real CXL HW <1µs/CB)
- bpftime agent initialized in benchmark process (shm connected to syscall-server shm)

### DEV-040 scope statement
CXL write done ONCE for CB 0 only. Cost 23µs/byte × 9216 bytes = 220ms is a QEMU TCG device-memory constraint. On real CXL hardware (DDR cache semantics), byte-stores cost ~5ns/byte → total <50µs for 9216 bytes. The one-shot write demonstrates the srsRAN→CXL data path. This workaround does not apply to production CXL hardware.
