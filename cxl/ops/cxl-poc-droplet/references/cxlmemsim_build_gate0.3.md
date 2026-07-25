# CXLMemSim build notes + Gate 0.3 outcome

## Build

```bash
git clone https://github.com/SlugLab/CXLMemSim \
    /root/linux_env/cxl/third_party/CXLMemSim --depth=1
cd /root/linux_env/cxl/third_party/CXLMemSim
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# binary: build/cxlmemsim_legacy
```

## CLI (v4 spec vs actual binary)

The spec references an older CLI. The built binary uses different flags:

| Spec (v4 prompt) | Actual binary |
|------------------|---------------|
| `./cxlmemsim --latency=142 -- ./bench` | `./build/cxlmemsim_legacy -t "/path/to/bench" -l 142,142 -c 0` |

- `-t TARGET`: target binary path (string, no args supported in simple mode)
- `-l READ_NS,WRITE_NS`: injected latency in nanoseconds
- `-c CPUSET`: CPU set to bind to

## Gate 0.3 outcome — FAIL (WSL2 no PMU)

CXLMemSim builds successfully (cmake exits 0, binary runs). However:

```
Error: perf_event_open failed for generic hardware cache misses:
       No such file or directory (errno=2)
```

WSL2 runs under Hyper-V which does not pass Intel PEBS hardware performance
counters to the guest kernel. `perf_event_open(PERF_TYPE_HARDWARE, ...)` fails.

**This is a hard WSL2 constraint.** CXLMemSim requires Intel PEBS for its
cache-miss injection mechanism.

## Workaround for droplet runs

The DigitalOcean Haswell droplet exposes Intel PMU via KVM. CXLMemSim should
work there. Steps:

```bash
# On the droplet (bare metal, not inside QEMU VM):
./build/cxlmemsim_legacy -t "/path/to/ldpc_bench" -l 0,0 -c 0    # baseline
./build/cxlmemsim_legacy -t "/path/to/ldpc_bench" -l 142,142 -c 0 # Pond CXL
./build/cxlmemsim_legacy -t "/path/to/ldpc_bench" -l 300,300 -c 0 # max sweep

# Do NOT run inside QEMU VM — nested PMU access may fail.
```

Sweep points: 0, 100, 142, 200, 255, 300 ns (matching Phase 5 latency_ladder spec).

## Downstream impact

Phase 4's 6-point CXL latency sweep (`cxl_latency_sensitivity.csv`) is deferred.
The `+gpu_compute_full × cxlmemsim_sweep` row in `latency_ladder_v2.csv` has
`source=deferred`. See DEV-005 in DEVIATIONS.md.
