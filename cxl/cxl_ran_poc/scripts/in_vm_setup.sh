#!/bin/bash
# Run INSIDE the QEMU VM (as root) after first boot.
# Brings up the CXL Type-3 device, creates a region, and reconfigures
# it as system-RAM so /dev/dax0.0 is accessible to the GPU daemon.
#
# Usage:
#   sudo /opt/cxl_ran_poc/scripts/in_vm_setup.sh
#
# Idempotent — safe to re-run.

set -euo pipefail

LOG_DIR="/var/log/cxl_ran_poc"
mkdir -p "$LOG_DIR"
LOGFILE="${LOG_DIR}/cxl_setup.log"
exec > >(tee -a "$LOGFILE") 2>&1

echo "=== CXL RAN PoC — in-VM setup  $(date -Iseconds) ==="

# ── 1. check we are inside the VM (kernel 6.x required) ─────────────────────
KERNEL=$(uname -r)
MAJOR=$(echo "$KERNEL" | cut -d. -f1)
if [[ "$MAJOR" -lt 6 ]]; then
    echo "ERROR: kernel ${KERNEL} < 6.0 — CXL driver not present." >&2
    echo "       Are you running inside the QEMU VM?" >&2
    exit 1
fi
echo "Kernel: ${KERNEL}  ✓"

# ── 2. ensure linux-modules-extra is installed (has cxl_core etc.) ───────────
if ! find /lib/modules/"$(uname -r)"/kernel/drivers/cxl -name 'cxl_core*' \
        &>/dev/null 2>&1 | grep -q .; then
    echo "Installing linux-modules-extra-$(uname -r) (needed for CXL driver)..."
    apt-get install -y "linux-modules-extra-$(uname -r)" 2>&1 | tail -3
    depmod -a
fi

# ── 3. load CXL modules ──────────────────────────────────────────────────────
for mod in cxl_core cxl_port cxl_pci cxl_mem cxl_acpi; do
    if modprobe "$mod" 2>/dev/null; then
        echo "Module ${mod}: loaded"
    else
        echo "Module ${mod}: not available (skipping)"
    fi
done

# ── 4. wait for CXL device to appear ─────────────────────────────────────────
echo "Waiting for CXL mem device..."
for i in $(seq 1 20); do
    if cxl list -M 2>/dev/null | grep -q memdev; then
        echo "CXL memdev found."
        break
    fi
    sleep 1
    if [[ "$i" -eq 20 ]]; then
        echo "ERROR: No CXL memdev after 20 s." >&2
        echo "       Verify QEMU was launched with the -device cxl-type3 flag." >&2
        cxl list 2>&1 || true
        exit 1
    fi
done

cxl list

# ── 5. create CXL region (if not already present) ────────────────────────────
REGION=$(cxl list --regions 2>/dev/null \
    | python3 -c "import sys,json; r=json.load(sys.stdin); print(r[0]['region'] if r else '')" \
    2>/dev/null || true)

if [[ -z "$REGION" ]]; then
    echo "Creating CXL region..."
    # QEMU CXL Type-3 topology: root decoder0.0 → mem0, 2 GiB volatile RAM
    CREATE_OUT=$(cxl create-region -d decoder0.0 -t ram -m mem0 -s 2G 2>&1 || true)
    echo "${CREATE_OUT}"

    # Extract region name from the JSON output: {"region":"region0",...}
    REGION=$(echo "${CREATE_OUT}" | python3 -c \
        "import sys,json
data=sys.stdin.read()
# find first JSON object in output
start=data.find('{')
if start>=0:
    try:
        obj=json.loads(data[start:])
        print(obj.get('region',''))
    except: pass
" 2>/dev/null || true)

    # Fallback: re-query
    if [[ -z "$REGION" ]]; then
        REGION=$(cxl list --regions 2>/dev/null \
            | python3 -c "import sys,json; r=json.load(sys.stdin); print(r[0]['region'] if r else '')" \
            2>/dev/null || true)
    fi

    if [[ -z "$REGION" ]]; then
        echo "WARN: could not determine region name; proceeding with /dev/dax scan"
    else
        echo "Region created: ${REGION}"
    fi
else
    echo "Region already exists: ${REGION}"
fi

# ── 6. reconfigure dax device (devdax → devdax mode is fine for mmap access) ──
# /dev/dax0.0 appears automatically after region commit. We keep it in devdax
# mode so the GPU daemon can mmap() it directly for CXL memory access.
# (system-ram mode would make it a NUMA node but requires memory hotplug)
for i in $(seq 1 10); do
    if [[ -c /dev/dax0.0 || -b /dev/dax0.0 ]]; then break; fi
    sleep 1
done

# ── 7. verify /dev/dax0.0 is accessible ──────────────────────────────────────
if [[ -c /dev/dax0.0 ]]; then
    echo "✓  /dev/dax0.0 is present (char device)"
    ls -la /dev/dax0.0
elif [[ -b /dev/dax0.0 ]]; then
    echo "✓  /dev/dax0.0 is present (block device)"
else
    echo "WARN: /dev/dax0.0 not found — GPU daemon will fall back to mmap-shm"
    echo "      Check: cxl list && daxctl list"
fi

# ── 8. NUMA topology ─────────────────────────────────────────────────────────
if command -v numactl &>/dev/null; then
    echo ""
    echo "=== NUMA topology ==="
    numactl --hardware
fi

# ── 9. write status file ─────────────────────────────────────────────────────
STATUS_FILE="${LOG_DIR}/cxl_setup_status.txt"
if [[ -c /dev/dax0.0 || -b /dev/dax0.0 ]]; then
    echo "qemu-cxl-type3" > "${STATUS_FILE}"
    echo "SETUP_OK $(date -Iseconds)" >> "${STATUS_FILE}"
else
    echo "mmap-shm-fallback" > "${STATUS_FILE}"
    echo "SETUP_PARTIAL $(date -Iseconds) — no /dev/dax0.0" >> "${STATUS_FILE}"
fi

echo ""
echo "=== Setup complete. Run in_vm_run.sh to start the pipeline. ==="
