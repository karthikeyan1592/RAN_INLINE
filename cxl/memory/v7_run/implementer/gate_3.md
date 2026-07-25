## Spec

Gate 3 — bpftime LLR mover: uprobe fires → LLR lands on CXL NUMA node 1 → OpenCL reads from CXL

Criteria:
- (a) bpftime uprobe handler fires at ldpc_decoder_impl::decode() entry (not kernel BPF)
- (b) LLR[0..4] values are in ±5..±20 range (real AWGN LDPC channel LLR, not zeros, not ±127)
- (c) CXL buffer confirms on NUMA node 1 (`get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE)` = 1)
- (d) OpenCL reads from CXL buffer (USE_HOST_PTR on node-1 allocation)
- (e) `[gate3] GATE3 PASS` printed by llr_gate3

DEV-003 ghost: SPSC pattern (single bpftime handler writes, one consumer reads) means
no torn reads possible without atomic. Resolved by design — state explicitly.

Mechanism note: this gate uses bpftime (userspace BPF via LD_PRELOAD), NOT kernel BPF.
The benchmark runs with `LD_PRELOAD=libbpftime-agent.so`. The llr_gate3 consumer runs with
`LD_PRELOAD=libbpftime-syscall-server.so`. This is the architectural claim: intercept a
running srsRAN process in userspace, no kernel module modification.

DEV-037 fallback: if bpftime uprobe attach fails (ENOSYS/EPERM), fall back to kernel tracefs
uprobe for the hook mechanism while keeping bpftime BPF map for data transport. Document as
DEV-037 if this fallback is invoked.

## Commands

```bash
# Build (inside VM):
cd /root/cxl/cxl_ran_poc/phase5_cxl
make gate3 BPFTIME_V7=/root/cxl/third_party/bpftime

# Verify BPF object:
llvm-readelf -S llr_mover.bpf.o | grep -E "uprobe|maps|Name|SEC"

# Run Gate 3 (inside VM, as root):
bash run_gate3_v7.sh --gate 3
```

Full manual steps (two terminals inside VM):

Terminal 1:
```bash
BENCH=$(find /root/cxl/third_party/srsRAN_Project/build -name ldpc_decoder_benchmark -type f | head -1)
AGENT=/root/cxl/third_party/bpftime/build/runtime/agent/libbpftime-agent.so
LD_PRELOAD=$AGENT "$BENCH" -L 384 -I 5 -T avx2 -R 100
```

Terminal 2:
```bash
SERVER=/root/cxl/third_party/bpftime/build/runtime/syscall-server/libbpftime-syscall-server.so
cd /root/cxl/cxl_ran_poc/phase5_cxl
BENCH_PID=$(pgrep -x ldpc_decoder_benchmark | head -1)
LD_PRELOAD=$SERVER ./llr_gate3 $BENCH_PID
```

Expected key output lines from llr_gate3:
```
[gate3] uprobe_offset=0xXXXXX
[gate3] bpftime_handler_attached pid=XXXXX
[gate3] LLR captured seq=1
[gate3] LLR[0..4]=<v0> <v1> <v2> <v3> <v4>
[gate3] cxl_buf ptr=0xXXXX numa_node=1 cxl_node=YES
[gate3] ocl_popcount=XXXX
[gate3] GATE3 PASS
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW — both terminals if applicable -->

```

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) bpftime uprobe fires (not kernel BPF) | PENDING | |
| (b) LLR[0..4] in ±5..±20 | PENDING | |
| (c) cxl_node=YES (numa_node=1) | PENDING | |
| (d) OCL ran on CXL buffer | PENDING | |
| (e) GATE3 PASS printed | PENDING | |
| DEV-003 resolution (SPSC no-torn-read) | PENDING | state explicitly |

**Overall: PENDING**
