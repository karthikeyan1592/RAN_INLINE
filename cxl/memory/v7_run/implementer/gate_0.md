## Spec

Gate 0 — KVM hypervisor confirmed inside QEMU VM

Criterion:
- (a) `dmesg` inside VM shows `Hypervisor detected: KVM` (not TCG, not None)
- (b) `/proc/cpuinfo` shows `hypervisor` flag

## Commands

```bash
# Inside VM (ssh root@localhost -p 2222)
dmesg | grep -i "hypervisor\|kvm"
grep -m1 "hypervisor" /proc/cpuinfo
virt-what 2>/dev/null || systemd-detect-virt
```

## Raw evidence

<!-- PASTE VERBATIM TERMINAL OUTPUT BELOW — no paraphrasing, no summary -->

```
$ dmesg | grep -i "hypervisor\|kvm"

```

<!-- END RAW EVIDENCE -->

## Self-verdict

| Criterion | Status | Evidence |
|-----------|--------|----------|
| (a) dmesg shows KVM | PENDING | paste line from Raw evidence |
| (b) /proc/cpuinfo hypervisor flag | PENDING | paste line from Raw evidence |

**Overall: PENDING** — raw evidence required
