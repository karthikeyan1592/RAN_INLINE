# Gate 0.3 — CXLMemSim build + smoke test

## Spec (verbatim from cursor_cxl_poc_prompt_v4.md)

```
## 0.3 — CXLMemSim build + smoke test

git clone https://github.com/SlugLab/CXLMemSim.git
cd CXLMemSim
mkdir build && cd build
cmake .. && make -j$(nproc)

# Smoke test against a trivial memory-bound loop (e.g. a pointer-chase
# over a large array), at --latency=0 and --latency=142:
./cxlmemsim --latency=0   -- ./your_membench
./cxlmemsim --latency=142 -- ./your_membench
# Confirm the 142ns run shows HIGHER latency than the 0ns run by a
# measurable, consistent amount across repeated runs.

### GATE 0.3
PASS if: CXLMemSim builds, runs, and the latency=142 run is
         measurably and consistently slower than latency=0.

FAIL  -> spend at most 1 hour on build issues (check issue tracker
         for your exact kernel/perf_event_open permissions — CXLMemSim
         typically needs perf counter access). If still failing,
         DECISION: Phase 4's latency-sensitivity figure is deferred
         to Limitations/Future Work (IISc real hardware), and Phase 4
         proceeds WITHOUT the CXLMemSim sweep — but Part C's real
         drivers/cxl/ validation (already done) still stands as the
         CXL-kernel-path result. Document which path in
         emulation_mode.txt.
```

## Commands run

```bash
# 2026-06-15 06:58  clone
cd /root/linux_env/cxl/third_party
git clone --depth 1 https://github.com/SlugLab/CXLMemSim.git
# EXIT=0, 631MB

# 2026-06-15 07:00  install spdlog (only missing dep)
apt-get install -y libspdlog-dev   # EXIT=0

# 2026-06-15 07:00  build
cd CXLMemSim
cmake -S . -B build
cmake --build build -j4
# EXIT=0 — built: cxlmemsim_legacy, cxlmemsim_server, cxlmemsim_latency

# 2026-06-15 08:30  baseline membench (pointer-chase, 128MB, no cxlmemsim)
gcc -O2 -o /tmp/membench /tmp/membench.c
/tmp/membench
# -> pointer_chase_ns_per_access: 189.68  (total 6.364 s)

# 2026-06-15 08:32  first attempt: cxlmemsim_legacy -t "/tmp/membench" -l 0,0 -c 0
./build/cxlmemsim_legacy -t "/tmp/membench" -l 0,0 -c 0 2>&1

# 2026-06-15 08:33  second attempt after perf_event_paranoid = -1:
echo -1 > /proc/sys/kernel/perf_event_paranoid
./build/cxlmemsim_legacy -t "/tmp/membench" -l 0,0 -c 0 2>&1
./build/cxlmemsim_legacy -t "/tmp/membench" -l 142,142 -c 0 2>&1
```

## Raw evidence

Baseline membench:
```
pointer_chase_ns_per_access: 189.68  (total 6.364 s, sink=7185229)
```

cxlmemsim_legacy latency=0 (first attempt, perf_event_paranoid=2):
```
use cpuid: 0 0
[2026-06-15 08:32:47.127] [error] [monitor.cpp:578] Failed to initialize PEBS for tgid=877852, tid=877852: perf_event_open failed for generic hardware cache misses. Check CPU PMU support and kernel.perf_event_paranoid: No such file or directory
EXIT0=1
```

After echo -1 > /proc/sys/kernel/perf_event_paranoid:
```
-1
=== retry latency=0 with paranoid=-1 ===
use cpuid: 0 0
[2026-06-15 08:33:28.241] [error] [monitor.cpp:578] Failed to initialize PEBS for tgid=878435, tid=878435: perf_event_open failed for generic hardware cache misses. Check CPU PMU support and kernel.perf_event_paranoid: No such file or directory
EXIT0=1
=== retry latency=142 ===
use cpuid: 0 0
[2026-06-15 08:33:28.243] [error] [monitor.cpp:578] Failed to initialize PEBS for tgid=878437, tid=878437: perf_event_open failed for generic hardware cache misses. Check CPU PMU support and kernel.perf_event_paranoid: No such file or directory
EXIT142=1
```

Root cause: WSL2 kernel 6.6.114.1-microsoft-standard-WSL2 does not expose Intel
PEBS/hardware PMU events. `perf_event_open(PERF_TYPE_HW_CACHE, ...)` returns
ENOENT ("No such file or directory") regardless of perf_event_paranoid.
Hyper-V virtualizes the CPU but does not pass hardware performance counters
through to the guest. This is a fundamental WSL2 constraint, not a config issue.

perf not even installed for this kernel:
```
WARNING: perf not found for kernel 6.6.114.1-microsoft
  You may need to install the following packages for this specific kernel:
    linux-tools-6.6.114.1-microsoft-standard-WSL2
    linux-cloud-tools-6.6.114.1-microsoft-standard-WSL2
```

## Self-reported verdict

FAIL — CXLMemSim builds (EXIT=0) but cannot run: requires hardware PMU
(Intel PEBS) which is unavailable in WSL2. The spec's "1 hour build issue"
budget was NOT exhausted (WSL2 PMU absence is well-known/unfix-able at the
tool level). The spec's FAIL path applies:

> DECISION: Phase 4's latency-sensitivity figure is deferred to
> Limitations/Future Work (IISc real hardware), and Phase 4 proceeds WITHOUT
> the CXLMemSim sweep — but Part C's real drivers/cxl/ validation (already
> done) still stands as the CXL-kernel-path result.

Document in emulation_mode.txt. Phase 1-3 continue unblocked.

## Deviations from spec

- Spec says `./cxlmemsim --latency=N`, but the tool's CLI uses `-l N,N` with
  target via `-t`. The flag `--latency` does not exist in the built binary.
  Used the actual CLI (`cxlmemsim_legacy -t TARGET -l NS,NS -c 0`). Logged
  as DEV-004.
- Spec assumes CXLMemSim will work; it does not on WSL2. This is the spec's
  predicted failure mode, not an unexpected one — the spec includes the FAIL
  path and deferral explicitly.

## Files produced/modified

- third_party/CXLMemSim/ (cloned, built; binary: build/cxlmemsim_legacy)
- memory/v4_run/implementer/phase0/gate_0.3_cxlmemsim.md (this file)
- (emulation_mode.txt to be written in Phase 4)

## Timestamp

2026-06-15T08:35:00Z
