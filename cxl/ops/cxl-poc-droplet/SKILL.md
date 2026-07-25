# Skill: cxl-poc-droplet

Lifecycle management for the AI-RAN CXL PoC research environment on
DigitalOcean. Encodes the exact QEMU+KVM+CXL configuration discovered
through debugging so each session is mechanical, not exploratory.

## Quick start

```bash
cd cxl/ops/cxl-poc-droplet

# 1. Provision (idempotent — reuses existing droplet)
./scripts/provision.sh

# 2. Install all research dependencies (once per fresh droplet)
./scripts/install_deps.sh

# 3. Check current state
./scripts/status.sh

# 4. Launch QEMU CXL VM inside the droplet
ssh root@$(cat .cxl_droplet_ip)
./scripts/qemu_cxl_launch.sh /path/to/disk.qcow2

# 5. Verify CXL stack inside VM
./scripts/verify_cxl_checks.sh    # from inside the droplet

# 6. End of session: checkpoint, then teardown
./scripts/checkpoint.sh
./scripts/teardown.sh
```

## Scripts

| Script | What it does |
|--------|-------------|
| `provision.sh` | Create or reuse `cxl-poc` droplet; verify KVM+AVX2 |
| `install_deps.sh` | Install QEMU, eBPF, OpenCL, srsRAN deps, Python |
| `qemu_cxl_launch.sh <disk>` | Boot QEMU with exact verified CXL flags |
| `verify_cxl_checks.sh` | 5 PASS/FAIL CXL checks inside running VM |
| `checkpoint.sh` | Shutdown + snapshot the droplet |
| `restore.sh [--yes]` | Print (or run) restore from latest snapshot |
| `teardown.sh` | Delete the droplet (confirm by name) |
| `delete_snapshot.sh [id]` | Delete a snapshot |
| `status.sh` | List droplets, snapshots, cost reminder |

## Critical QEMU flags

```
-enable-kvm -cpu host,-hypervisor -M q35,cxl=on
```

`,-hypervisor` is mandatory. Without it `cxl create-region` fails
with ENXIO. See `references/cxl_qemu_kvm_gotchas.md`.

## Environment variables

All `provision.sh` defaults are overridable:

```bash
CXL_DROPLET_NAME=cxl-poc        # droplet name
CXL_DROPLET_REGION=blr1         # DO region
CXL_DROPLET_SIZE=s-4vcpu-8gb    # droplet size
CXL_DROPLET_IMAGE=ubuntu-24-04-x64
CXL_SSH_KEY_ID=<auto>           # from doctl ssh-key list
```

## bpftime / OAI runtime setup

```bash
# 1. Network namespaces for rfsimulator (WSL2 / bare-metal)
sudo ./scripts/setup_netns.sh          # creates gnb-ns + ue-ns with veth pair

# 2. Start bpftime consumer (MUST be first)
ip netns exec gnb-ns \
  LD_PRELOAD=.../libbpftime-syscall-server.so \
  BPFTIME_VM_NAME=ubpf SPDLOG_LEVEL=warn \
  ./ldpc_measure &
# wait for "probes attached" in consumer log

# 3. Start gNB with bpftime agent inside gnb-ns
ip netns exec gnb-ns bash -c "cd /tmp/oai_run && \
  LD_PRELOAD=.../libbpftime-agent.so \
  .../nr-softmodem -O gnb.conf --phy-test --rfsim --noS1 2>&1 &"

# 4. Wait ~5s for gNB PHY init, then start UE
sleep 5
ip netns exec ue-ns \
  .../nr-uesoftmodem --rfsimulator.serveraddr 10.77.0.2 \
    --reconfig-file /tmp/oai_run/reconfig.raw \
    --rbconfig-file /tmp/oai_run/rbconfig.raw \
    --phy-test --noS1 2>&1 &
# UE PHY sync takes 10-15s — wait 45s before declaring timeout
```

See `references/bpftime_build_gate0.1.md`, `references/oai_build_notes.md`.

## XDP observation (Gate 3b)

```bash
# Attach to veth-gnb inside gnb-ns (separate from uprobe — no interference)
# Compile from ebpf/xdp_rfsim_observe.bpf.c (requires vmlinux.h from bpftime)
ip netns exec gnb-ns \
  ./nic_timeline_consumer  # attaches xdp_rfsim_observe to veth-gnb, writes CSV
```

Source: `ebpf/xdp_rfsim_observe.bpf.c`.

## References

| File | Contents |
|------|---------|
| `references/bpftime_build_gate0.1.md` | bpftime cmake flags, runtime order, Gate 0.1 PASS (248.5 ns/call) |
| `references/cxlmemsim_build_gate0.3.md` | CXLMemSim build, Gate 0.3 FAIL (WSL2 no PMU), droplet workaround |
| `references/oai_build_notes.md` | OAI build, uprobe offsets, reconfig.raw fix, UE sync timing |
| `references/cxl_qemu_kvm_gotchas.md` | QEMU CXL flags, hypervisor bit, persistent-memdev path |

## Verified environment

- Provider: DigitalOcean BLR1, s-4vcpu-8gb
- OS: Ubuntu 24.04 (host) + 6.8.0-71-generic
- QEMU: 8.2.2
- KVM: /dev/kvm present, AVX2 exposed to guest
- CXL: 2 NUMA nodes, node1=1920MB, dax0.0 in system-ram mode
