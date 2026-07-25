# Gate 0.1 — bpftime smoke test (CRITICAL FIRST CHECK)

## Spec (verbatim from cursor_cxl_poc_prompt_v4.md)

```
### GATE 0.1
PASS if: bpftime builds, attaches, and bpftime_uprobe overhead is
         BOTH lower than 9,941ns AND stable across 3 repeated runs
         (stddev < mean — i.e. NOT the "sigma > mean" noise pattern
         from the WSL2 eBPF measurement).

FAIL  -> DECISION: bpftime is NOT the interception mechanism for this
         project. Phase 2 uses BPF_MAP_TYPE_RINGBUF kernel uprobes
         instead. The 9,941ns number is then the HONEST per-CB
         interception cost — record it as
         `interception_overhead_ns: 9941, method: kernel_uprobe,
          source: measured (D1)` in calibration_check.txt. This is
         FINE. Document the bpftime attempt and failure reason in
         ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md so
         nobody retries it blind.

Either outcome is a PASS for the overall project — write down which
one, then move on. Do not spend more than the time-box on this.
```

Recording spec (verbatim):
```
Record in `paper/results/bpftime_smoke.csv`:
method,overhead_ns
kernel_uprobe_baseline,9941   # from D1, for reference
bpftime_uprobe,<measured>
```

## Commands run

```bash
# 2026-06-15 06:35  clone bpftime (pinned at the commit below)
cd /root/linux_env/cxl/third_party
git clone --depth 1 --recurse-submodules --shallow-submodules \
  https://github.com/eunomia-bpf/bpftime.git
git -C bpftime log --oneline -1
#   d3918c9 fix(syscall-server): fix race condition in initialize_ctx() ... (#563)

# 2026-06-15 06:38  build bpftime (release, ubpf JIT — lighter than LLVM JIT)
cd bpftime
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBPFTIME_LLVM_JIT=0
cmake --build build --config Release -j$(nproc)
#   -> BUILD_EXIT=0, [100%]; produced:
#      build/runtime/syscall-server/libbpftime-syscall-server.so
#      build/runtime/agent/libbpftime-agent.so

# 2026-06-15 06:40  build the minimal example's libbpf+bpftool toolchain
cd example/minimal && make uprobe victim     # EXIT=0

# 2026-06-15 06:42  smoke target: no-op target_func() called 100k/loop
#   (test_target.c, gcc -O2 -g). Baseline (no uprobe):
cd /root/linux_env/cxl/third_party/bpftime_smoke
gcc -O2 -g -o test_target test_target.c
./test_target 3
#   -> run 0/1/2: 0.5 ns/call (baseline, no uprobe attached)

# 2026-06-15 06:43  bpftime uprobe probe on test_target:target_func
#   smoke.bpf.c: BPF_MAP_TYPE_ARRAY counter, increments on each call.
#   Compiled with the minimal example's libbpf.a + bpftool skeleton.

# FIRST ATTEMPT FAILED (documented, see Deviations / runtime.log):
#   (a) default VM = "llvm" but build used ubpf -> "No VM factory
#       registered for name: llvm" -> probe never instantiated,
#       call_count = 0.  FIX: export BPFTIME_VM_NAME=ubpf
#   (b) ubpf VM has no atomic opcode -> "unknown opcode 0xdb at PC 9"
#       (from __sync_fetch_and_add). FIX: plain `*v += 1` (victim is
#       single-threaded), rebuild.

# 2026-06-15 06:45  working run: loader under syscall-server, then
#   test_target x3 under the agent, BPFTIME_VM_NAME=ubpf
export SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf
LD_PRELOAD=.../libbpftime-syscall-server.so ./smoke_loader &   # registers probe
for run in 1 2 3; do LD_PRELOAD=.../libbpftime-agent.so ./test_target 3; done
# then SIGINT loader -> dumps call_count
```

## Raw evidence

Baseline (no uprobe), `./test_target 3`:
```
run 0: 0.5 ns/call (total 0.047 ms, sink=1410165408)
run 1: 0.5 ns/call (total 0.047 ms, sink=2115248112)
run 2: 0.5 ns/call (total 0.046 ms, sink=-1474636480)
```

Loader attach (raw, from loader.out):
```
libbpf: elf: symbol address match for 'target_func' in '/root/linux_env/cxl/third_party/bpftime_smoke/test_target': 0x1350
smoke loader: probe attached, waiting (Ctrl-C to dump)
```

bpftime uprobe ATTACHED, `./test_target 3` x3 invocations (raw):
```
===== bpftime-attached run 1 =====
run 0: 246.7 ns/call (total 24.666 ms, sink=1410165408)
run 1: 254.7 ns/call (total 25.472 ms, sink=2115248112)
run 2: 248.0 ns/call (total 24.802 ms, sink=-1474636480)
===== bpftime-attached run 2 =====
run 0: 252.0 ns/call (total 25.199 ms, sink=1410165408)
run 1: 254.0 ns/call (total 25.404 ms, sink=2115248112)
run 2: 252.1 ns/call (total 25.208 ms, sink=-1474636480)
===== bpftime-attached run 3 =====
run 0: 237.1 ns/call (total 23.714 ms, sink=1410165408)
run 1: 246.8 ns/call (total 24.678 ms, sink=2115248112)
run 2: 245.0 ns/call (total 24.499 ms, sink=-1474636480)
```

Event-count correctness (the probe really fired, not just plumbing):
expected = 3 invocations x (100k warmup + 3x100k measured) = 1,200,000.
```
$ grep call_count loader.out
call_count = 1200000
```

Statistics over the 9 attached samples:
```
n=9 mean=248.5 stdev=5.2 min=237.1 max=254.7
stddev<mean? True
overhead vs baseline(0.5): 248.0 ns/call
kernel uprobe baseline: 9941 ns -> bpftime is 40.0x lower
```

bpftime_smoke.csv (cxl_ran_poc/paper/results/bpftime_smoke.csv):
```
method,overhead_ns,stddev_ns,n_samples,source
kernel_uprobe_baseline,9941,,,measured (D1, prior session, WSL2 int3+KVM VM-exit)
bpftime_uprobe,248,5.2,9,measured (Gate 0.1, 2026-06-15, WSL2 userspace Frida uprobe)
```

## Self-reported verdict

PASS -> bpftime IS the interception mechanism for Phase 2.

bpftime builds (BUILD_EXIT=0), attaches (symbol resolved at 0x1350,
call_count=1,200,000 == expected), and bpftime_uprobe overhead
(248.5 ns/call) is BOTH lower than 9,941 ns (40x lower) AND stable
across 3 runs (stddev 5.2 ns << mean 248.5 ns — the opposite of the
"sigma > mean" WSL2 kernel-eBPF noise pattern). All three PASS clauses
satisfied.

## Deviations from spec

1. Built bpftime with the ubpf JIT (`-DBPFTIME_LLVM_JIT=0`) and ran
   with `BPFTIME_VM_NAME=ubpf` rather than the default LLVM JIT — see
   DEV-002.
2. Probe uses a non-atomic counter increment (`*v += 1`) instead of
   `__sync_fetch_and_add`, because the ubpf VM has no atomic opcode
   (0xdb). Safe here (single-threaded victim) — see DEV-003. If Phase 2
   needs an atomic (multi-threaded gNB), revisit: either switch to the
   LLVM JIT build or use a per-CPU map.

## Files produced/modified

- third_party/bpftime/ (cloned, built; pinned commit d3918c9)
- third_party/bpftime_smoke/test_target.c, smoke.bpf.c, smoke.c,
  smoke_loader, test_target (smoke harness)
- cxl_ran_poc/paper/results/bpftime_smoke.csv
- memory/v4_run/implementer/phase0/gate_0.1_bpftime.md (this file)

## Timestamp

2026-06-15T06:48:00Z
