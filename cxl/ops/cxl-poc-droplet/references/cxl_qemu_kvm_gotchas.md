# CXL + QEMU + KVM Gotchas

## 1. The kernel gate: `cpu_cache_has_invalidate_memregion()`

Every `cxl create-region` commit passes through this check at
`drivers/cxl/core/region.c:131` (Linux 6.8):

```c
cpu_cache_has_invalidate_memregion()
  → return !X86_FEATURE_HYPERVISOR
         || (hypervisor_is_type(X86_HYPER_MS_HYPERV)
             && ms_hyperv.features & HV_X64_MSR_CR_TLB_FLUSH);
```

The intent: on bare metal the CPU can invalidate its own caches before
the CXL region activates. Under a hypervisor the guest CPU cannot do
this safely unless the hypervisor specifically advertises the
`HV_X64_MSR_CR_TLB_FLUSH` capability. Currently only Microsoft
Hyper-V is in the allowlist. KVM is not (as of Linux 6.8) — this is
a real gap in upstream KVM CXL support, not a kernel bug per se.

When the check returns false, `commit_store()` fires a kernel WARN
and returns `-ENXIO`:

```
cxl region0: Failed to synchronize CPU cache state
WARNING: CPU: 3 PID: 124 at drivers/cxl/core/region.c:131
cxl create-region: failed to commit decode: No such device or address
```

## 2. The fix: `-cpu host,-hypervisor`

Removing the `X86_FEATURE_HYPERVISOR` CPUID bit with `,-hypervisor`
satisfies the first condition (`!X86_FEATURE_HYPERVISOR = true`), so
the entire OR short-circuits to `true` without needing the Hyper-V
allowlist. KVM acceleration stays on and all host CPU features
(including AVX2) pass through untouched — this is a CPUID flag
removal only, not a mode change.

**Confirmed working (DigitalOcean BLR1, s-4vcpu-8gb, Ubuntu 24.04):**
```
-enable-kvm -cpu host,-hypervisor -M q35,cxl=on
```
Result (serial log excerpt):
```
[    1.797628]  pci0000:34: host supports CXL
cxl create-region: exit=0   (decode_state=commit, dpa_resource=0x0)
ndctl create-namespace: exit=0
daxctl reconfigure-device --mode=system-ram: exit=0
available: 2 nodes (0-1)
  node 0 size: 3915 MB   (host DRAM)
  node 1 size: 1920 MB   (CXL persistent memory, online)
  node distances: 0→0=10, 0→1=20
```

## 3. What does NOT work — do not retry these

### WSL2 (Hyper-V Level-1 guest)
`/dev/kvm` is absent — Hyper-V does not re-expose VMX/SVM to guests.
Forced to TCG mode. TCG is ~1,400× slower than native. AVX2 in TCG
makes the kernel boot use YMM for memset/memcpy, taking minutes.
srsRAN's `ldpc_decoder_benchmark` binary contains 5,278 YMM
instructions and SIGILLs under any TCG CPU model without AVX2,
even with `-Tgeneric` (the flag controls the LDPC algorithm, not
instruction selection in the binary).

Workaround used during development: `-cpu qemu64,-hypervisor` removes
the hypervisor bit (CXL works) but provides no AVX2, so srsRAN still
SIGILLs. All WSL2 QEMU measurements are invalid for paper use.

### `-cpu host` alone on KVM (without `,-hypervisor`)
The guest CPUID reports `X86_FEATURE_HYPERVISOR` set. The kernel
identifies the hypervisor as KVM (not Hyper-V), so the second OR
condition is false. Result: same WARN, same ENXIO. Verified on
DigitalOcean droplet with `/dev/kvm` present.

### `volatile-memdev` for `cxl-type3` in QEMU 8.2
Using `volatile-memdev=` instead of `persistent-memdev=` leaves
`decoder2.0` with `dpa_resource=0x0` but `target_type=expander`
and `mode=none` — the endpoint decoder DPA is never properly
initialized. Any subsequent `cxl create-region` or `ndctl
create-namespace` fails or produces an unusable region.

Fix: use `persistent-memdev` (file-backed) + a RAM-backed LSA:
```
-object memory-backend-file,id=cxl-mem0,share=on,
  mem-path=/tmp/cxl_mem.img,size=2G,align=256M
-object memory-backend-ram,id=cxl-lsa0,size=256M
-device cxl-type3,bus=rp0,
  persistent-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-pmem0
```
Then convert to system-ram via `daxctl reconfigure-device
--mode=system-ram dax0.0` (requires kmem module and
`auto_online_blocks=offline` beforehand).

## 4. Expected-good state after full CXL bring-up

```
$ numactl --hardware
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3
node 0 size: 3915 MB
node 1 cpus:
node 1 size: 1920 MB
node distances:
node   0   1
  0:  10  20
  1:  20  10

$ ls -la /dev/dax0.0
crw------- 1 root root 510, 0 ... /dev/dax0.0

$ cxl list -M
[{ "memdev":"mem0", "pmem_size":2147483648, "host":"0000:35:00.0" }]

$ cat /sys/bus/cxl/devices/decoder2.0/dpa_resource
0x0                           ← committed, not sentinel (0xffffffffffffffff)
```

## 5. Alternative to `-hypervisor`: `CONFIG_CXL_REGION_INVALIDATION_TEST`

The CXL core contains a built-in bypass for exactly this scenario
(`drivers/cxl/core/region.c`, `cxl_region_invalidate_memregion()`):
when `CONFIG_CXL_REGION_INVALIDATION_TEST=y`, a failed
`cpu_cache_has_invalidate_memregion()` check is bypassed with a `WARN_ONCE`
instead of returning `-ENXIO`, and `CXL_REGION_F_INCOHERENT` is cleared.
This does **not** require hiding the hypervisor CPUID bit and has no side effects
on other guest code paths.

**Verified 2026-06-14** on Ubuntu Noble 24.04, kernel 6.8.0-71-generic (DigitalOcean BLR1):
```
grep CONFIG_CXL_REGION_INVALIDATION_TEST /boot/config-$(uname -r)
# CONFIG_CXL_REGION_INVALIDATION_TEST is not set
```

Result: **not set**. Ubuntu generic kernels do not enable this debug option,
so `-cpu host,-hypervisor` remains the only practical choice for KVM-hosted
CXL emulation on Ubuntu. If you build a custom kernel with this option enabled,
plain `-cpu host` (without `,-hypervisor`) may work for `cxl create-region`.

## 6. virtio devices must use `bus=pcie.0`

When using `-M q35,cxl=on`, QEMU creates a `pxb-cxl` bus alongside `pcie.0`.
Any virtio device specified without an explicit `bus=` parameter may be routed
to the CXL bus, causing:
```
PCI: Only PCI/PCIe bridges can be plugged into pxb-cxl
```

**Fix:** replace `-drive if=virtio` with explicit device declarations:
```bash
-drive file="$DISK",if=none,id=disk0,format=qcow2
-device virtio-blk-pci,drive=disk0,bus=pcie.0
-device virtio-net-pci,netdev=net0,bus=pcie.0
```

## 7. `cxl-fmw.0.interleave-ways` unsupported in QEMU 8.2

QEMU 8.2 does not support the `cxl-fmw.0.interleave-ways` machine parameter.
Specifying it causes:
```
Parameter 'cxl-fmw.0.interleave-ways' is unexpected
```
Remove this parameter. Single-device CXL windows work correctly without it:
```
-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G
```

## 8. CET-SHSTK incompatibility with CXL-backed NUMA node

`numactl --membind=<cxl-node>` (strict binding) causes SIGILL in
`ld-linux-x86-64.so.2` at ELF offset `0x1d292` on kernels with
`CONFIG_X86_USER_SHADOW_STACK=y` (Ubuntu Noble, kernel 6.8). The dynamic
linker's shadow stack page allocation fails when forced onto the CXL-backed
(`memory-backend-file`) NUMA node.

**Root cause**: The binary has GNU property `x86 feature: IBT, SHSTK`. With
`MPOL_BIND` set to node 1, the kernel cannot allocate shadow stack pages with
the required attributes from the QEMU `memory-backend-file` CXL region.

**Workaround**: LD_PRELOAD an `mbind()` shim that redirects allocations ≥16 KB
to the CXL node while keeping small allocations (including the shadow stack) on
node 0/DRAM. See `paper/results/` directory for `node1_mbind.c` source.

```c
/* compiled on droplet, SCPed to VM as /tmp/node1_mbind.so */
void *malloc(size_t size) {
    void *p = real_malloc(size);
    if (p && size >= 16384)
        mbind(page_align(p), round_up(size), MPOL_BIND, &node1_mask, 2,
              MPOL_MF_MOVE | MPOL_MF_STRICT);
    return p;
}
```
Usage: `LD_PRELOAD=/tmp/node1_mbind.so <benchmark>`

**Open question**: Does this SIGILL also occur on real CXL hardware? Real CXL
Type-3 persistent memory exposes different page attributes than QEMU's
`memory-backend-file`. If future testing on real CXL hardware (e.g., IISc HACC)
is planned, this should be retested — it may be a QEMU-specific limitation, or
it could reveal a real kernel gap in CET-SHSTK support for CXL-backed NUMA nodes.
