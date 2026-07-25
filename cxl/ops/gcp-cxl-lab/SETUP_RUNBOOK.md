# GCP CXL Lab — Environment Setup Runbook

**Last updated:** 2026-06-25  
**Session:** v7 GCP first run  
**Target machine:** cxl-systems-lab, asia-south2-a, project cxl-systems-lab-26  
**IP:** 34.131.224.105 (static external)  
**SSH:** `ssh -i ~/.ssh/id_ed25519 karthix25@34.131.224.105`

---

## Layer 0 — GCP Instance (one-time, already done)

```bash
# Created via Cloud Shell:
gcloud compute instances create cxl-systems-lab \
  --project=cxl-systems-lab-26 \
  --zone=asia-south2-a \
  --machine-type=n2-standard-4 \
  --min-cpu-platform="Intel Cascade Lake" \
  --enable-nested-virtualization \
  --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud \
  --boot-disk-size=40GB \
  --boot-disk-type=pd-standard

# Billing: 01A962-376712-6E289B
# IP recorded in: ops/gcp-cxl-lab/.gcp_instance_ip
```

**Verify KVM:**
```bash
grep -c vmx /proc/cpuinfo   # expect >= 1 (was 8)
ls /dev/kvm                 # expect present
```

---

## Layer 1 — GCP Host Packages (done once, survives reboots)

```bash
ssh karthix25@34.131.224.105
sudo apt-get update
sudo apt-get install -y \
  qemu-system-x86 qemu-kvm qemu-utils cloud-image-utils \
  build-essential cmake ninja-build git clang llvm \
  libbpf-dev linux-tools-$(uname -r) \
  pocl-opencl-icd ocl-icd-opencl-dev clinfo \
  numactl ndctl daxctl \
  libelf-dev libssl-dev flex bison libncurses-dev \
  python3-pip \
  libboost-all-dev libconfig++-dev libyaml-cpp-dev \
  libfftw3-dev libgtest-dev libzmq3-dev \
  libsctp-dev libjsoncpp-dev \
  meson libtraceevent-dev libtracefs-dev libkeyutils-dev \
  uuid-dev libiniparser-dev libudev-dev libjson-c-dev \
  libkmod-dev libsystemd-dev

# Add user to kvm group (required to launch QEMU without sudo):
sudo usermod -aG kvm karthix25
# Takes effect on next SSH session; workaround: sg kvm -c "..."
```

---

## Layer 2 — Source Tree (rsync from local)

```bash
# On LOCAL machine:
rsync -az --exclude '*/build' --exclude '*/build_*' --exclude '.git' \
  --exclude '__pycache__' --exclude '*.o' --exclude '*.a' --exclude '*.so' \
  /root/linux_env/cxl/ karthix25@34.131.224.105:/home/karthix25/cxl/
```

**Result:** source at `~/cxl/` on GCP host.

---

## Layer 3 — bpftime Build (GCP host, Ubuntu 24.04)

**CRITICAL PATCH** — the bpftool submodule has absolute symlinks to
`/root/linux_env/...` that don't exist on GCP. Patch before cmake:

```bash
cd /home/karthix25/cxl/third_party/bpftime

# Patch cmake/libbpf.cmake to skip bundled bpftool, use system bpftool:
python3 - << 'PYEOF'
import re
path = "cmake/libbpf.cmake"
with open(path) as f:
    txt = f.read()

# Replace ExternalProject_Add(bpftool ...) block with system bpftool
old_block = re.search(
    r'ExternalProject_Add\(bpftool.*?set\(BPFTOOL_INSTALL_DIR.*?\)',
    txt, re.DOTALL
)
if old_block:
    replacement = '''find_program(BPFTOOL_BINARY bpftool REQUIRED)
set(BPFTOOL_INSTALL_DIR /usr/sbin)
add_custom_target(bpftool)  # no-op so dependents build'''
    txt = txt[:old_block.start()] + replacement + txt[old_block.end():]
    with open(path, 'w') as f:
        f.write(txt)
    print("Patch applied")
else:
    print("Pattern not found — check if already patched")
PYEOF

# Build:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
# Expected artifacts:
#   build/libruntime.a
#   build/runtime/agent/libbpftime-syscall-server.so
```

**Verified offset for THIS build:**
```bash
nm build/runtime/agent/libbpftime-syscall-server.so | grep ldpc_decoder_impl.*decode
# GCP fresh build offset: 0x30cf0  (DO droplet was 0x35280 — different!)
# Document per-build. Re-derive if binary is rebuilt.
```

---

## Layer 4 — srsRAN LDPC Benchmark (GCP host)

```bash
cd /home/karthix25/cxl/third_party/srsRAN_Project
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_EXPORT=ON \
      -DBUILD_TESTING=ON -DASSERT_LEVEL=MINIMAL
cmake --build build -j4 --target ldpc_decoder_benchmark

# Verify:
./build/tests/benchmarks/phy/upper/channel_coding/ldpc/ldpc_decoder_benchmark \
  -L 384 -Tavx2 -R 5
# Expected: runs, prints throughput, no SIGILL
```

---

## Layer 5 — QEMU VM Disk Image

```bash
# Download Ubuntu 22.04 cloud image:
wget -O /tmp/cxl_vm.qcow2 \
  https://cloud-images.ubuntu.com/jammy/current/jammy-server-cloudimg-amd64.img
qemu-img resize /tmp/cxl_vm.qcow2 15G

# Create CXL backing file:
truncate -s 2G /tmp/cxl_mem.img

# Create cloud-init seed ISO (inject VM SSH key):
# vm_key was generated on GCP host: ssh-keygen -t ed25519 -f ~/.ssh/vm_key
mkdir -p /tmp/cidata

# user-data content (vm_key.pub = the key generated on GCP host):
VM_KEY_PUB="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOnuj6Cl/6/AmaunydkilSsokyZGIqjLZUKGIdqSkyp9 karthix25@cxl-systems-lab"
python3 - << PYEOF
vm_key = "$VM_KEY_PUB"
user_data = f"""#cloud-config
disable_root: false
ssh_pwauth: true
users:
  - name: root
    ssh_authorized_keys:
      - {vm_key}
    lock_passwd: false
chpasswd:
  expire: false
  list: |
    root:cxlroot
runcmd:
  - systemctl enable ssh
  - systemctl start ssh
"""
with open("/tmp/cidata/user-data", "w") as f:
    f.write(user_data)
with open("/tmp/cidata/meta-data", "w") as f:
    f.write("instance-id: cxl-lab-vm-v2\nlocal-hostname: cxl-vm\n")
PYEOF

cloud-localds /tmp/cidata.iso /tmp/cidata/user-data /tmp/cidata/meta-data
```

---

## Layer 6 — Launch QEMU CXL VM

**Command (run as karthix25 via `sg kvm`):**

```bash
nohup sg kvm -c "qemu-system-x86_64 \
  -enable-kvm \
  -cpu host,-hypervisor \
  -smp 4 \
  -m 4G,slots=8,maxmem=16G \
  -M q35,cxl=on \
  -drive file=/tmp/cxl_vm.qcow2,if=none,id=disk0,format=qcow2 \
  -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
  -drive file=/tmp/cidata.iso,if=none,id=cidata,media=cdrom,format=raw \
  -device virtio-blk-pci,drive=cidata,bus=pcie.0 \
  -object memory-backend-file,id=cxl-mem0,share=on,mem-path=/tmp/cxl_mem.img,size=2G,align=256M,pmem=off \
  -object memory-backend-ram,id=cxl-lsa0,size=256M \
  -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 \
  -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 \
  -device cxl-type3,bus=rp0,persistent-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-pmem0 \
  -M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,bus=pcie.0 \
  -nographic -serial mon:stdio" > /tmp/qemu.log 2>&1 &
echo $! > /tmp/qemu.pid
```

**SSH into VM (from GCP host):**
```bash
ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost
```

**Wait for boot:**
```bash
until ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no \
  -o ConnectTimeout=5 -o BatchMode=yes root@localhost true 2>/dev/null; do
  sleep 5; printf .
done; echo VM up
```

**Key flags explained:**
- `-cpu host,-hypervisor`: exposes host CPU features; `-hypervisor` hides hypervisor bit (required for CXL cache-sync)
- `persistent-memdev=cxl-mem0`: CXL device reports PMEM capacity
- `pmem=off` on memory-backend-file: changes file backend from pmem to regular (reduces WC mapping issues, though not fully — see DEV-033)
- `bus=pcie.0` on all virtio devices: required with q35+cxl=on (CXL bus is pxb-cxl, not the default)

---

## Layer 7 — VM: Kernel Upgrade to 6.8 HWE

Ubuntu 22.04 cloud image ships kernel 5.15 which lacks full CXL module support.

```bash
# Inside VM:
apt-get install -y linux-image-generic-hwe-22.04 linux-headers-generic-hwe-22.04

# Set 6.8 as default boot kernel:
grub-set-default "gnulinux-advanced-5267ed34-510f-43e4-9251-beddb1550c4e>gnulinux-6.8.0-124-generic-advanced-5267ed34-510f-43e4-9251-beddb1550c4e"
update-grub
reboot

# After reboot, verify:
uname -r  # expect: 6.8.0-124-generic
```

**Note:** The GRUB UUID (`5267ed34-...`) is filesystem-specific. If using a fresh image,
look it up with: `grep "menuentry\|submenu" /boot/grub/grub.cfg | head -10`

---

## Layer 8 — VM: CXL Kernel Modules + ndctl v80

### 8a. Load CXL modules
```bash
modprobe cxl_acpi cxl_pci cxl_mem cxl_pmem
lsmod | grep cxl  # expect: cxl_core, cxl_mem, cxl_pci, cxl_acpi, cxl_port, cxl_pmem
```

### 8b. Install numactl + other tools
```bash
apt-get install -y numactl libnuma-dev ndctl daxctl \
  linux-modules-extra-6.8.0-124-generic \
  pocl-opencl-icd ocl-icd-opencl-dev clinfo \
  build-essential cmake ninja-build git clang llvm \
  libboost-all-dev libconfig++-dev libyaml-cpp-dev \
  libfftw3-dev libgtest-dev libzmq3-dev \
  libssl-dev libelf-dev flex bison \
  python3-pip libjsoncpp-dev libsctp-dev \
  libtraceevent-dev libtracefs-dev libkeyutils-dev uuid-dev \
  linux-tools-$(uname -r) linux-tools-generic
```

### 8c. Build ndctl v80 from source (Ubuntu 22.04 ships 72.1 which lacks `cxl create-region`)

```bash
cd /tmp
git clone --depth=1 --branch v80 https://github.com/pmem/ndctl.git
cd ndctl

# Fix iniparser header path for --prefix=/usr/local:
mkdir -p /usr/local/include/iniparser
cp /usr/include/iniparser/*.h /usr/local/include/iniparser/

meson setup build --prefix=/usr/local --libdir=/usr/local/lib -Ddocs=disabled
ninja -C build -j4

# Install binaries and libraries manually (ninja install may not set symlinks):
cp build/cxl/cxl /usr/local/bin/cxl
cp build/cxl/lib/libcxl.so.1.0.7     /usr/local/lib/libcxl.so.1.0.7
cp build/daxctl/lib/libdaxctl.so.1.0.6 /usr/local/lib/libdaxctl.so.1.0.6
cp build/ndctl/lib/libndctl.so.6.4.21  /usr/local/lib/libndctl.so.6.4.21
rm -f /usr/local/lib/libcxl.so.1 /usr/local/lib/libdaxctl.so.1 /usr/local/lib/libndctl.so.6
ln -s /usr/local/lib/libcxl.so.1.0.7 /usr/local/lib/libcxl.so.1
ln -s /usr/local/lib/libdaxctl.so.1.0.6 /usr/local/lib/libdaxctl.so.1
ln -s /usr/local/lib/libndctl.so.6.4.21 /usr/local/lib/libndctl.so.6
ldconfig

/usr/local/bin/cxl version  # expect: 80
```

---

## Layer 9 — VM: CXL Region + NUMA Node 1

Run this every time the VM boots (state is not persistent across QEMU restarts):

```bash
# 1. Load modules
modprobe cxl_acpi cxl_pci cxl_mem cxl_pmem

# 2. Create PMEM region
/usr/local/bin/cxl create-region -m mem0 -d decoder0.0
# Expected: region0, resource=0x690000000, size=2GB, type=pmem

# 3. Create devdax namespace
ndctl create-namespace --mode=devdax
# Expected: namespace0.0, chardev=dax0.0

# 4. Reconfigure as system-ram (adds NUMA node 1)
echo offline > /sys/devices/system/memory/auto_online_blocks
daxctl reconfigure-device --mode=system-ram --no-online dax0.0

# 5. Online the memory blocks
daxctl online-memory dax0.0

# 6. Verify
numactl --hardware
# Expected:
#   available: 2 nodes (0-1)
#   node 0: ~3915 MB  (DRAM)
#   node 1: ~1920 MB  (CXL, distance=20)
```

**Verification script:** `~/cxl/ops/cxl-poc-droplet/scripts/verify_cxl_checks.sh`

---

## Known Issues / Deviations from v7 Prompt

### DEV-031: numactl --membind=1 SIGILL (process-wide binding)
- **Symptom:** `numactl --membind=1 ls` → exit 132 (SIGILL), even with `pmem=off`
- **Cause:** CXL PMEM region iomem-typed as "Persistent Memory"; kernel maps early
  process pages (stack, loader) with WC (Write-Combining) attribute. The first
  `rep stosb`/`rep movsb` in glibc's `_start` on WC memory causes `#UD` → SIGILL.
- **Impact on PoC:** NONE. The PoC uses `mbind()` on specific shared buffers only,
  not process-wide binding.
- **Proof:** `numa_alloc_onnode(size, 1)` works. `memfd + mmap + mbind(MPOL_BIND, node1)
  + memset` works. `numa_maps` confirms `N1=<n>`.
- **Workaround for verify_cxl_checks.sh check 5:** replace `numactl --membind=1 ls`
  with the mbind_test C program.

### DEV-032: ndctl 72.1 missing `cxl create-region`
- Ubuntu 22.04 ships ndctl 72.1 which lacks `cxl create-region`.
- Fix: built ndctl v80 from source (see Layer 8c above).
- Binary at `/usr/local/bin/cxl`.

### DEV-033: CXL region sysfs target write EINVAL/ENXIO
- When setting region0/target0 manually via sysfs, EINVAL occurs if decoder mode is
  wrong, ENXIO if DPA is not pre-allocated.
- Fix: use `/usr/local/bin/cxl create-region` which handles DPA allocation internally.

### DEV-034: grub-set-default UUID is image-specific
- The GRUB UUID (`5267ed34-510f-43e4-9251-beddb1550c4e`) is the root filesystem UUID
  of this specific qcow2 image. On a fresh image it will differ.
- Fix: after boot, run `grep submenu /boot/grub/grub.cfg` and extract the UUID for
  the 6.8 entry before calling grub-set-default.

### DEV-035: bpftime bpftool cmake patch
- bpftool submodule in bpftime source has absolute symlinks to local build machine path.
- Fix: replace ExternalProject_Add(bpftool) in cmake/libbpf.cmake with find_program().
- The patch is applied to the file in-tree at `third_party/bpftime/cmake/libbpf.cmake`.

---

## Quick Reconnect Checklist

After any interruption, to get back to working state:

```bash
# 1. SSH to GCP host
ssh -i ~/.ssh/id_ed25519 karthix25@34.131.224.105

# 2. Check if QEMU is running
pgrep qemu-system && echo running || echo stopped

# 3. If stopped, relaunch (Layer 6 command above)

# 4. SSH into VM
ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost

# 5. Check CXL state — if numactl shows only 1 node, re-run Layer 9
numactl --hardware

# 6. If only 1 node: run Layer 9 setup sequence
```

---

## File Locations

| What | Where |
|------|-------|
| GCP ops scripts | `ops/gcp-cxl-lab/scripts/` |
| QEMU launch script | `ops/cxl-poc-droplet/scripts/qemu_cxl_launch.sh` |
| CXL verify checks | `ops/cxl-poc-droplet/scripts/verify_cxl_checks.sh` |
| GCP IP | `ops/gcp-cxl-lab/.gcp_instance_ip` |
| VM SSH key (on GCP host) | `~/.ssh/vm_key` (NOT in repo) |
| CXL backing file | `/tmp/cxl_mem.img` (on GCP host, ephemeral) |
| VM disk image | `/tmp/cxl_vm.qcow2` (on GCP host, ephemeral) |
| cloud-init seed ISO | `/tmp/cidata.iso` (on GCP host) |
| ndctl v80 source | `/tmp/ndctl/` (inside VM) |
| ndctl v80 binary | `/usr/local/bin/cxl` (inside VM) |
