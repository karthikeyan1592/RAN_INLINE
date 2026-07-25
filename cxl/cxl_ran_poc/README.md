# CXL + eBPF + L1 RAN Offload PoC

Open, vendor-neutral proof-of-concept for transparent 5G NR L1 compute offload
(LDPC, FFT) using Linux CXL emulation, eBPF uprobes, and a CPU-based accelerator
daemon (PoCL/OpenCL-ready path).

## Architecture

1. **L1 simulator** (`l1_sim/ran_l1_sim`) — synthetic NR slot workload
2. **eBPF uprobes** (`ebpf/`) — intercept `ldpc_decode()` / `fft_process()`
3. **GPU daemon** (`gpu_daemon/`) — accelerator compute on CXL shared memory
4. **Offload shim** (`offload/libl1_offload.so`) — weak-symbol transparent routing

## Quick start

```bash
make all
./scripts/verify.sh
SLOTS=1000 ./scripts/run_poc.sh
```

## vs NVIDIA Aerial

| Aspect | This PoC | NVIDIA Aerial |
|--------|----------|---------------|
| L1 changes | None (uprobe + weak shim) | CUDA integration in OAI |
| Memory | CXL Type-3 / NUMA / mmap | GPU HBM |
| Runtime | Linux kernel + eBPF | CUDA proprietary |

## Emulation honesty

- QEMU CXL Type-3 is **functionally correct**, not timing-accurate
- Latency sensitivity uses NUMA binding or CXLMemSim (see `scripts/numa_sweep.sh`)
- GPU speedup is **projected** from arXiv:2602.04652 (~6×), not measured here

## Hardware

- Minimum: 8+ cores, 16 GB RAM for full measurement
- QEMU 9.x+ for CXL Type-3 in VM
- Kernel 6.6+ recommended (5.15+ works with ringbuf, no user_ringbuf)

## Results

CSV output: `paper/results/`
Figures: `paper/figures/`
