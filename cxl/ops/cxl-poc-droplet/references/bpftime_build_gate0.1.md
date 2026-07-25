# bpftime build notes + Gate 0.1 outcome

## Build (from v4 session, 2026-06-15)

```bash
git clone https://github.com/eunomia-bpf/bpftime \
    /root/linux_env/cxl/third_party/bpftime --depth=1
cd /root/linux_env/cxl/third_party/bpftime
git submodule update --init --recursive

# IMPORTANT: build with ubpf JIT, NOT LLVM (DEV-002)
# Without this, runtime errors: "No VM factory registered for name: llvm"
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBPFTIME_LLVM_JIT=0 \
    -DBPFTIME_BUILD_STATIC_LIB=1
cmake --build build -j$(nproc)
```

## Key build output paths

```
build/runtime/syscall-server/libbpftime-syscall-server.so
build/runtime/agent/libbpftime-agent.so
build/libbpf/libbpf.a
build/libbpf/          (headers)
build/bpftool/bpftool
third_party/vmlinux/   (vmlinux.h for BPF programs)
```

## Runtime usage pattern

```bash
# 1. Start consumer (syscall-server) FIRST
LD_PRELOAD=.../libbpftime-syscall-server.so \
  SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf \
  ./consumer_program &

# 2. Start target process (agent) SECOND
LD_PRELOAD=.../libbpftime-agent.so \
  SPDLOG_LEVEL=warn \
  ./target_binary &
```

Order is mandatory: agent must find a running syscall-server at connect time.

## Gate 0.1 outcome — PASS

Measurement: bpftime uprobe round-trip latency baseline.

```
N=1,200,000 calls
mean = 248.5 ns/call
(Cloudflare kernel-eBPF reference: 1,670 ns/call — bpftime is 6.7× lower)
```

Source: `paper/results/bpftime_smoke.csv`.

## Known issues / deviations

- **DEV-002**: Must use `BPFTIME_VM_NAME=ubpf`. Default "llvm" JIT fails on this build.
- **DEV-003** (CLOSED by DEV-013): Non-atomic increment rejected by ubpf (opcode 0xdb).
  Fix: use `BPF_MAP_TYPE_PERCPU_ARRAY` for any counter touched by concurrent threads.
- **Uprobe offsets**: Must be verified for each libldpc.so build.
  OAI offsets (v4 session): `nrLDPC_coding_decoder=0xe9b30`, `LDPCdecoder=0x63110`.
  Re-verify with: `nm -D libldpc.so | grep -E "nrLDPC_coding_decoder|LDPCdecoder"`.
- **PERCPU map userspace**: `bpf_map_lookup_elem` on `BPF_MAP_TYPE_PERCPU_ARRAY` from
  the syscall-server process returns zeroes. Aggregate in the agent-side BPF program
  or use the approach in Gate 3's `ldpc_consumer.c` (`libbpf_num_possible_cpus()`).
