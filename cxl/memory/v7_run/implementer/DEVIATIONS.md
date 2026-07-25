# Deviations Log — v7 GCP Run

Append-only. Continues from DEV-032 (last official entry in
`paper/results/droplet/gates/DEVIATIONS.md`).

**Cross-reference**: SETUP_RUNBOOK.md used labels "DEV-031" through "DEV-035" locally
(runbook-only labels). Those numbers are TAKEN in the official log. The entries below
are the official continuation.

---

## DEV-033 — numactl --membind=1 SIGILL on GCP KVM QEMU CXL VM

**Version**: v7  
**Gate**: 2, 3  
**Status**: RESOLVED (workaround in place)

**Description**: Running `numactl --membind=1 <command>` inside the QEMU CXL VM on GCP KVM
causes SIGILL. The CXL-backed pages receive Write-Combining (WC) cache attribute from ACPI
CEDT. SIGILL occurs during the mbind syscall path when WC semantics conflict with the program's
alignment assumptions. Adding `pmem=off` to the QEMU device line does not eliminate WC on GCP
(contrast: DO droplet may differ).

**Workaround**: Use `numa_alloc_onnode(size, 1)` directly from C code (bypasses numactl shim)
followed by `mbind()` with MPOL_BIND and nodemask={1}. This works because the C program has
no alignment traps around the syscall.

**Evidence**: gate_2.md raw evidence (DEV-030 seen on DO droplet; DEV-033 is GCP-specific
recurrence of same issue with different mitigation path).

---

## DEV-034 — ndctl 72.1 missing `cxl create-region`

**Version**: v7  
**Gate**: 2  
**Status**: RESOLVED (ndctl v80 source build)

**Description**: Ubuntu 22.04 ships ndctl 72.1. The `cxl create-region` subcommand required
to configure CXL memory regions was added in ndctl v76+. Attempting `cxl create-region` with
72.1 fails with `unknown command`.

**Resolution**: Build ndctl v80 from source inside the VM:
```
git clone --depth=1 --branch v80 https://github.com/pmem/ndctl.git
cd ndctl && meson setup build --prefix=/usr/local && ninja -C build && ninja -C build install
```
Script: `vm_install_ndctl80.sh` (idempotent, run once on first 6.8 boot).

---

## DEV-035 — bpftime cmake ExternalProject_Add(bpftool) absolute symlink to /root/linux_env

**Version**: v7  
**Gate**: 3  
**Status**: RESOLVED (find_program patch in build_tools.sh)

**Description**: bpftime's CMakeLists.txt uses `ExternalProject_Add(bpftool)` which creates
absolute symlinks pointing to `/root/linux_env/...`. On the GCP host (user karthix25, prefix
/home/karthix25/cxl/), these symlinks are dangling and break the bpftime build.

**Resolution**: Patch before cmake: replace `ExternalProject_Add(bpftool ...)` block with
`find_program(BPFTOOL_BINARY bpftool REQUIRED)` so cmake uses the system bpftool. Applied
automatically by `build_tools.sh` via Python string replacement.

---

## DEV-036 — GRUB set-default by UUID is image-specific

**Version**: v7  
**Gate**: 0  
**Status**: RESOLVED (kernel-version string selection)

**Description**: `prepare_vm.sh` set GRUB default via UUID from the cloud-init image. After
the HWE kernel install + reboot sequence, the UUID changed, causing the VM to boot the
original 5.x kernel instead of the newly installed 6.8 HWE kernel.

**Resolution**: `launch_vm.sh` selects the default kernel by kernel-version string
(`6.8.0-*-generic`) using `grub-set-default "$(grep -m1 '6.8.0' /boot/grub/grub.cfg)"` on
the cloud-init firstboot, not by UUID.

---

## DEV-037 — [RESERVED for Gate 3 bpftime uprobe attach issues]

If `bpf_program__attach_uprobe` under bpftime syscall-server returns ENOSYS or EPERM:
- Document exact error here
- Fall back to kernel tracefs uprobe for the hook mechanism
- Keep bpftime BPF map for LLR data transport (architectural claim preserved for map path)
- Gate 3 criterion (a) becomes PARTIAL: uprobe hook is kernel tracefs, map transport is bpftime

---
