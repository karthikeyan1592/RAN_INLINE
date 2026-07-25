# CXL RAN PoC — Results Summary (v7)

**Environment**: GCP n2-standard-4 (asia-south2-a) → QEMU CXL VM (kernel 6.8.0-124-generic)
**Date**: 2026-06-25
**PRIMARY_CONFIG anchor**: 11,703 µs/slot = 23.4× (fixed, never re-derived)

---

## Phase 2 — CXL Node 1 Allocation + OpenCL Sentinel

| Metric | Value |
|--------|-------|
| CXL node 1 allocated | PASS — `numa_alloc_onnode(64 KB, node 1)` |
| CXL read latency (CPU) | **34,329 ns** (~34 µs) |
| CXL write BW (64 KB, warm pages) | measured |
| OpenCL sentinel readback | **PASS** — `0xDEADBEEFCAFEBABE` via `CL_MEM_USE_HOST_PTR` |

Key implementation notes:
- `memfd + mmap + mbind` path was abandoned: WC-mapped CXL pages cause ~250 ms per 4 KB page fault under QEMU
- `numa_alloc_onnode(64 KB, 1)` is the working allocation path for CXL node 1
- `setvbuf(stdout, NULL, _IONBF, 0)` required for visibility through SSH pipe

---

## Phase 3 — BPF LLR Mover Compilation

| Artifact | Details |
|----------|---------|
| `ldpc_llr_mover.bpf.o` | 11 KB — kernel BPF uprobe program |
| `ldpc_llr_mover.skel.h` | 36 KB — bpftool-generated skeleton |
| `vmlinux.h` | 150,622 lines from `/sys/kernel/btf/vmlinux` |
| Uprobe target | `ldpc_decoder_impl::decode()` at file offset `0x2fef0` |
| LLR register mapping | `rdx` = span.data (PARM3), `rcx` = span.size (PARM4) |

Register mapping note: `decode(bit_buffer&, span<llr>, crc*, cfg&)` ABI:
- `rdi` = this, `rsi` = bit_buffer& (output), `rdx` = span.data, `rcx` = span.size
- Initial BPF used PARM2/PARM3 (wrong); corrected to PARM3/PARM4 after disasm verification

---

## Phase 4 — E2E Uprobe + BPF Map Readback

**Loader**: `ldpc_uprobe_loader.c` — kernel BPF (no bpftime needed for root+kernel 6.8)
Built against `bpftime/build/libbpf/libbpf/libbpf.a` (system libbpf 0.5 too old for BTF load)

| Metric | Value |
|--------|-------|
| uprobe fires | **4,080** |
| LLR bytes captured | **12,204,320 bytes** (~11.6 MB) |
| Last LLR slot size | 12,288 bytes |
| Sequence counter | 4,080 |
| ldpc_decoder_benchmark wall time | 10.70 s (20 reps, BG1+BG2 all lifting sizes) |
| BPF map type | `BPF_MAP_TYPE_ARRAY`, 1 slot × 12,304 bytes |

---

## Full E2E Data Path (verified working)

```
srsRAN ldpc_decoder_benchmark
  ↓  kernel uprobe @ decode() +0x2fef0
ldpc_llr_mover.bpf.o  →  BPF ARRAY map [llr_region]
  ↓  bpf_map_lookup_elem() from loader process
ldpc_uprobe_loader reads LLR bytes + stats
  ↓  (Phase 2 separately verified)
CXL node 1 (numa_alloc_onnode) → OpenCL CL_MEM_USE_HOST_PTR → PoCL readback PASS
  ↓
e2e_gcp.csv
```

---

## e2e_gcp.csv Contents

```
metric,value,unit
uprobe_hits,4080,count
llr_bytes_total,12204320,bytes
last_llr_len,12288,bytes
last_seq,4080,count
ldpc_wall_s,10.703,seconds
ldpc_reps,20,count
cxl_read_latency_ns,34329,ns
primary_config_us_slot,11703,us
primary_config_slowdown,23.4,x
```

---

## Key Constraints / Limitations

1. **QEMU CXL WC pages**: CXL window is marked Persistent Memory by ACPI CEDT → Write-Combining cache type. Each unfaulted 4 KB page costs ~250 ms. Max safe pre-fault = 64 KB.
2. **bpftime agent not used**: Kernel uprobe (root + kernel 6.8) is simpler and works. bpftime LD_PRELOAD agent (129 MB) was built but not required.
3. **System libbpf 0.5 incompatible**: Cannot load BPF objects compiled with `-g` (BTF). Used bpftime's bundled libbpf (newer) instead.
4. **LDPC generic decoder only**: VM does not have AVX2/AVX-512 instruction sets available under QEMU, so the benchmark uses the generic path (lower throughput than bare metal).

---

## DEV Sequence

- DEV-033: QEMU CXL VM setup (ndctl v80, cxl create-region, daxctl system-ram)
- DEV-034: Phase 2 CXL+OCL sentinel — WC-page issue discovered and fixed
- DEV-035: Phase 3 BPF compilation — vmlinux.h, ldpc_llr_mover.bpf.o, skel.h
- DEV-036: Phase 4 E2E — uprobe loader, register fix (PARM2→PARM3, PARM3→PARM4), 4080 hits
