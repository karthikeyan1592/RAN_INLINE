# Superseded Results

The `numa_*_software-synthetic-wsl2.csv` files were generated using a
**software-synthetic** NUMA latency model running on WSL2 (Hyper-V guest).
They were intended to model CXL access latency sensitivity at 0/142/255 ns.

**Superseded by:** `three_way_cxl_comparison.csv` (Part C, 2026-06-14)

Part C ran the identical AVX2 srsRAN LDPC benchmark inside a real QEMU/KVM
CXL Type-3 emulation (q35, pxb-cxl, persistent-memdev), with the CXL region
appearing as NUMA node 1 (distance=20) inside the VM. The result showed that
QEMU 8.2's `memory-backend-file` backend does not model CXL.mem access latency:
node 0 (DRAM) and node 1 (CXL emulated) produced statistically indistinguishable
results (median p50 delta = 53 µs/CB, within ±100 µs run-to-run noise band).

These files are retained here for provenance only. Do not cite as CXL latency
sensitivity results — use `three_way_cxl_comparison.csv` and the
`cxl_kernel_path.pdf` figure instead.
