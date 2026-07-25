## Spec

Gate 2 — CXL NUMA topology confirmed in VM

Criteria:
- (a) `numactl --hardware` shows at least 2 nodes
- (b) node 1 = 1920 MB (QEMU persistent-memdev, 2G minus ACPI overhead)
- (c) `daxctl list` shows the region in system-ram mode (not devdax)
- (d) Allocation on node 1 succeeds and reads back at correct address

DEV-030 note: `numactl --membind=1` causes SIGILL on GCP QEMU with WC-mapped CXL pages.
Workaround: use `numa_alloc_onnode()` + `mbind()` shim (as in gate0_option_a.c and llr_gate3.c).
DEV-033: same issue on GCP KVM; pmem=off does not eliminate WC; mbind shim still required.

## Commands

```bash
# Inside VM:
numactl --hardware
daxctl list 2>/dev/null || ndctl list -D
numactl --cpunodebind=0 --membind=1 dd if=/dev/zero of=/dev/null bs=4k count=1 2>&1 || \
  echo "DEV-030/DEV-033: membind SIGILL — use gate0_option_a instead"

# Run gate0_option_a (numa_alloc_onnode + mbind shim):
cd /root/cxl/cxl_ran_poc/phase5_cxl
./gate0_option_a
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW -->

```

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) 2+ NUMA nodes | PENDING | |
| (b) node 1 = 1920 MB | PENDING | |
| (c) daxctl system-ram mode | PENDING | |
| (d) alloc on node 1 OK | PENDING | |

**Overall: PENDING**
