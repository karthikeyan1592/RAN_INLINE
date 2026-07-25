#!/bin/bash
# vm_cxl_setup.sh — run INSIDE the QEMU VM after every boot to bring up CXL NUMA node 1
#
# Idempotent: safe to run multiple times.
# Requires: kernel 6.8+, /usr/local/bin/cxl (ndctl v80), ndctl, daxctl
#
# Usage (from GCP host):
#   ssh -i ~/.ssh/vm_key -p 2222 -o StrictHostKeyChecking=no root@localhost \
#     "bash /root/cxl/ops/gcp-cxl-lab/scripts/vm_cxl_setup.sh"
set -euo pipefail

CXL_BIN="${CXL_BIN:-/usr/local/bin/cxl}"
export PATH=/usr/local/bin:$PATH

echo "=== vm_cxl_setup.sh ==="
echo "kernel: $(uname -r)"

# Ensure numactl is available (not always pre-installed in cloud images)
command -v numactl >/dev/null || apt-get install -y -qq numactl libnuma-dev 2>/dev/null

# ── Step 1: Load CXL modules ──────────────────────────────────────────────────
echo "Loading CXL modules..."
for mod in cxl_acpi cxl_pci cxl_mem cxl_pmem; do
  modprobe "$mod" 2>/dev/null && echo "  $mod: loaded" || echo "  $mod: already loaded"
done

# ── Step 2: Check if region already committed ─────────────────────────────────
if [ -f /sys/bus/cxl/devices/region0/commit ] && \
   [ "$(cat /sys/bus/cxl/devices/region0/commit 2>/dev/null)" = "1" ]; then
  echo "region0 already committed — skipping create-region"
else
  echo "Creating CXL PMEM region..."
  "$CXL_BIN" create-region -m mem0 -d decoder0.0 2>&1 | head -5
fi

# ── Step 3: Create devdax namespace if not present ────────────────────────────
if daxctl list 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); exit(0 if d else 1)" 2>/dev/null; then
  echo "dax device already exists — skipping namespace creation"
else
  echo "Creating devdax namespace..."
  ndctl create-namespace --mode=devdax 2>&1 | head -3
fi

# ── Step 4: Reconfigure to system-ram if not already ─────────────────────────
DAX_MODE=$(daxctl list 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print(d[0].get('mode','?'))" 2>/dev/null || echo "?")
if [ "$DAX_MODE" = "system-ram" ]; then
  echo "dax0.0 already in system-ram mode"
else
  echo "Reconfiguring dax0.0 → system-ram..."
  echo offline > /sys/devices/system/memory/auto_online_blocks
  daxctl reconfigure-device --mode=system-ram --no-online dax0.0 2>&1 | head -3
fi

# ── Step 5: Online memory blocks ─────────────────────────────────────────────
ONLINE=$(daxctl list 2>/dev/null | python3 -c \
  "import sys,json; d=json.load(sys.stdin); print(d[0].get('online_memblocks',0))" 2>/dev/null || echo "0")
TOTAL=$(daxctl list 2>/dev/null | python3 -c \
  "import sys,json; d=json.load(sys.stdin); print(d[0].get('total_memblocks',0))" 2>/dev/null || echo "0")

if [ "$ONLINE" = "$TOTAL" ] && [ "$TOTAL" != "0" ]; then
  echo "All $TOTAL memory blocks already online"
else
  echo "Onlining $TOTAL CXL memory blocks (currently $ONLINE online)..."
  daxctl online-memory dax0.0 2>&1 | head -3
fi

# ── Step 6: Verify ────────────────────────────────────────────────────────────
echo ""
echo "=== NUMA topology ==="
numactl --hardware

NODE1_MB=$(numactl --hardware 2>/dev/null | awk '/node 1 size:/ {print $4}')
if [ -n "$NODE1_MB" ] && [ "$NODE1_MB" -gt 0 ]; then
  echo ""
  echo "PASS: CXL NUMA node 1 = ${NODE1_MB} MB"
else
  echo ""
  echo "FAIL: node 1 has no memory — check dmesg and repeat"
  exit 1
fi

# ── Step 7: Quick mbind smoke test ────────────────────────────────────────────
python3 -c "
import ctypes
libnuma = ctypes.CDLL('libnuma.so.1')
libnuma.numa_alloc_onnode.restype = ctypes.c_void_p
ptr = libnuma.numa_alloc_onnode(ctypes.c_size_t(4096), ctypes.c_int(1))
if not ptr:
    raise RuntimeError('numa_alloc_onnode returned NULL')
buf = (ctypes.c_char * 4096).from_address(ptr)
buf[0:4] = b'\xca\xfe\xba\xbe'
assert buf[0:4] == b'\xca\xfe\xba\xbe', 'write/read mismatch'
libnuma.numa_free(ctypes.c_void_p(ptr), ctypes.c_size_t(4096))
print('PASS: mbind smoke test — write/read node1 OK')
" || echo "FAIL: mbind smoke test"

echo ""
echo "=== vm_cxl_setup.sh complete ==="
