# Gate 0.4 — Bonus: multi-socket droplet check (time-box: 15 minutes, do not exceed)

## Spec (verbatim from cursor_cxl_poc_prompt_v4.md)

```
### GATE 0.4 (informational only, does not block anything)
If found: note the droplet size/region in
  ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md as a
  potential future Pond-replication target (real cross-socket delta,
  expect 70-90ns per Cohet). Do NOT pursue further in this session —
  Phase 4's CXLMemSim sweep is the primary path regardless.

If not found (likely): note "checked, single-socket only" and move on.
```

(0.4 command spec, verbatim:)
```bash
# On 2-3 LARGER DigitalOcean droplet sizes (do not provision unless
# quick to check — if doctl/console makes this slow, SKIP entirely,
# this is a bonus not a requirement):
numactl --hardware
# Looking for: "available: 2 nodes" with DISTINCT physical nodes
# (not QEMU-labeled — this would be the HOST's own NUMA, visible
# before any QEMU involvement at all).
```

## Commands run

```bash
# 2026-06-15 06:50  (WSL2 host — local check; no new droplet provisioned
#                    per the "do not provision unless quick" instruction)
numactl --hardware
ls -d /sys/devices/system/node/node*
lscpu | grep -i numa
```

## Raw evidence

```
=== GATE 0.4: numactl --hardware ===
available: 1 nodes (0)
node 0 cpus: 0 1 2 3
node 0 size: 7943 MB
node 0 free: 1561 MB
node distances:
node   0
  0:  10

=== /sys NUMA nodes ===
/sys/devices/system/node/node0

=== lscpu NUMA ===
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-3
```

## Self-reported verdict

not found (single-socket only) — informational, does not block.

This WSL2 host is single-socket: `available: 1 nodes (0)`, only
`node0` present in /sys, distance matrix is the trivial `0: 10`. The
DigitalOcean droplet cxl-poc (s-4vcpu-8gb, per [[cxl-poc-environment]])
is likewise single-socket (basic droplet size). No larger droplet was
provisioned — the spec marks provisioning optional and "skip if slow",
and a multi-socket host is not on the critical path (Phase 4 uses
CXLMemSim software injection regardless).

## Deviations from spec

The spec phrases the check as run on "2-3 LARGER DigitalOcean droplet
sizes". I ran it on the WSL2 host only and did not provision larger
droplets, exercising the spec's own escape hatch ("do not provision
unless quick to check ... SKIP entirely, this is a bonus"). Logged as
DEV-001 in DEVIATIONS.md.

## Files produced/modified

- memory/v4_run/implementer/phase0/gate_0.4_multisocket.md (this file)
- (note for ops/cxl-poc-droplet/references/cxl_qemu_kvm_gotchas.md to be
  appended in Phase 6.4 skill-update step: "multi-socket check: WSL2 +
  droplet both single-socket; no Pond-replication target available")

## Timestamp

2026-06-15T06:52:00Z
