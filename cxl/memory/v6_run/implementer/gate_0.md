# Gate 0 — Mode Conflict Resolution

## Spec
Resolve DEV-023/DEV-024: system-ram mode (NUMA node 1 for numactl --membind) and devdax /dev/dax0.0
(for OpenCL CL_MEM_USE_HOST_PTR mmap) are mutually exclusive on a single CXL region.

PASS requires in ONE configuration:
1. Allocation physically on CXL NUMA node — confirmed via get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE)
2. CL_MEM_USE_HOST_PTR over SAME region succeeds, sentinel written CPU-side readable by CL kernel

## Option chosen: A — system-ram + numa_alloc_onnode + CL_MEM_USE_HOST_PTR

Single region stays in system-ram mode. Both OAI side (numactl --membind=1 or numa_alloc_onnode)
and OpenCL side use the same node-1 virtual address space. No devdax device needed.

## Commands

### VM setup (run once per boot)
```
# Load CXL modules
modprobe cxl_acpi cxl_pci cxl_mem cxl_pmem

# Create region if not exists
cxl create-region -m mem0 -d decoder0.0   # or skip if region0 already exists

# Create devdax namespace
ndctl create-namespace --mode=devdax

# Reconfigure to system-ram — NUMA node 1 comes online
daxctl reconfigure-device --mode=system-ram dax0.0

# Online memory blocks (automatic via daxctl but explicit here)
for f in /sys/devices/system/memory/memory*/state; do
  echo online > "$f" 2>/dev/null || true
done
```

### Gate 0 test
```
gcc -O2 -o gate0_option_a gate0_option_a.c -lnuma -lOpenCL
./gate0_option_a
```

## Raw evidence

```
=== NUMA ===
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3
node 0 size: 3915 MB   node 0 free: 688 MB
node 1 cpus:
node 1 size: 1920 MB   node 1 free: 1920 MB
node distances:
node   0   1 
  0:  10  20 
  1:  20  10 

=== Gate 0 Option A output ===
[gate0] NUMA max_node=1
[gate0] NUMA node 1 size=1920 MB
[gate0] allocating 1048576 bytes on NUMA node 1...
[gate0] PROOF1 ptr=0x76139bca2000 numa_node=1 cxl_node=YES exit=0
[gate0] wrote sentinel 0xCA7EBEEF at buf[0] (CPU-side, no CL write)
[gate0] OpenCL platform: Portable Computing Language
[gate0] OpenCL device:   cpu-haswell-DO-Regular
[gate0] PROOF2 clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0 (0=OK)
[gate0] sentinel_cpu=0xCA7EBEEF cl_out=0xCA7EBEEF match=YES
[gate0] GATE0 PASS: option=A zero_copy=YES numa_node=1
gate0_exit=0
```

## Self-verdict: PASS

Both criteria met in ONE configuration (system-ram mode, NUMA node 1 live):
1. PROOF1: get_mempolicy → numa_node=1, cxl_node=YES — allocation physically on CXL node
2. PROOF2: clCreateBuffer(CL_MEM_USE_HOST_PTR) err=0 — sentinel written CPU-side (no clEnqueueWriteBuffer)
   read by OpenCL kernel as 0xCA7EBEEF — confirmed zero-copy

Option A — no devdax needed. CXL region stays in system-ram mode throughout.
NUMA node 1 = 1920 MB CXL @ distance 20. OpenCL via PoCL (CPU backend, QEMU VM).

## Deviations
None for Gate 0. DEV-023/DEV-024 resolved by Option A: devdax abandoned, system-ram used for both sides.

## Files
- `cxl_ran_poc/phase5_cxl/gate0_option_a.c` — Gate 0 test program
