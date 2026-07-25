#!/bin/bash
# verify_cxl_checks.sh — run 5 CXL correctness checks inside a running QEMU VM
#
# Assumes: VM launched via qemu_cxl_launch.sh with -netdev hostfwd=tcp::2222-:22
# SSH must be reachable on localhost:2222 (cloud-init or pre-configured image).
#
# Testing note: validated against the manually-built initramfs
# (cxl_verify.cpio.gz from Step 2) via serial log inspection.
# For a cloud image, inject SSH key via cloud-init NoCloud user-data.
set -euo pipefail

VM_PORT="${CXL_VM_PORT:-2222}"
VM_HOST="${CXL_VM_HOST:-localhost}"
VM_KEY="${SSH_KEY:-/root/.ssh/vm_id_ed25519}"
KEY_OPT=""
[ -f "$VM_KEY" ] && KEY_OPT="-i $VM_KEY"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5 -p $VM_PORT $KEY_OPT"

echo "=== CXL Verification Checks (VM: $VM_HOST:$VM_PORT) ==="
echo ""

# ── Wait for VM SSH ───────────────────────────────────────────
echo "Waiting for VM SSH..."
for i in $(seq 1 40); do
  ssh $SSH_OPTS root@"$VM_HOST" true 2>/dev/null && break
  [ $i -eq 40 ] && { echo "ERROR: VM SSH not reachable on port $VM_PORT after 200s"; exit 1; }
  sleep 5
done
echo "VM SSH ready"
echo ""

PASS=0
FAIL=0
RESULTS=()

run_check() {
  local num="$1" label="$2" cmd="$3" expect="$4"
  local out rc=0
  out=$(ssh $SSH_OPTS root@"$VM_HOST" "$cmd" 2>&1) || rc=$?
  if echo "$out" | grep -qF "$expect" 2>/dev/null; then
    RESULTS+=("  [PASS] $num. $label")
    PASS=$((PASS+1))
  else
    RESULTS+=("  [FAIL] $num. $label")
    RESULTS+=("         expected: $expect")
    RESULTS+=("         got:      $(echo "$out" | head -3)")
    FAIL=$((FAIL+1))
  fi
}

# ── Check 1: hypervisor bit clear ────────────────────────────
run_check 1 "hypervisor bit clear" \
  'grep -c hypervisor /proc/cpuinfo 2>/dev/null || echo 0' \
  "0"

# ── Check 2: no cache-sync warning ───────────────────────────
# Negative check: expect NO output from dmesg grep
OUT2=$(ssh $SSH_OPTS root@"$VM_HOST" \
  'dmesg | grep -i "Failed to synchronize CPU cache state" || true' 2>&1)
if [ -z "$OUT2" ]; then
  RESULTS+=("  [PASS] 2. no cache-sync warning in dmesg")
  PASS=$((PASS+1))
else
  RESULTS+=("  [FAIL] 2. no cache-sync warning in dmesg")
  RESULTS+=("         found: $OUT2")
  FAIL=$((FAIL+1))
fi

# ── Check 3: cxl region exists (create if needed) ────────────
OUT3=$(ssh $SSH_OPTS root@"$VM_HOST" 'bash -s' << 'INNER' 2>&1
modprobe cxl_acpi cxl_pmem cxl_port cxl_mem 2>/dev/null || true
# Accept existing region as success — "No space left" means region0 already occupies mem0
EXISTING=$(cxl list -R 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print(d[0]['region'] if d else '')" 2>/dev/null || echo "")
if [ -n "$EXISTING" ]; then
  echo "existing region: $EXISTING"
  echo "exit=0"
else
  cxl create-region -m mem0 -d decoder0.0 2>&1
  echo "exit=$?"
fi
INNER
)
if echo "$OUT3" | grep -q "exit=0"; then
  RESULTS+=("  [PASS] 3. cxl region exists ($(echo "$OUT3" | grep -o 'region[0-9]*' | head -1))")
  PASS=$((PASS+1))
else
  RESULTS+=("  [FAIL] 3. cxl create-region failed")
  RESULTS+=("         $(echo "$OUT3" | tail -3)")
  FAIL=$((FAIL+1))
fi

# ── Check 4: daxctl system-ram reconfigure ───────────────────
OUT4=$(ssh $SSH_OPTS root@"$VM_HOST" 'bash -s' << 'INNER' 2>&1
ndctl create-namespace --mode=devdax 2>/dev/null || true
echo offline > /sys/devices/system/memory/auto_online_blocks 2>/dev/null || true
daxctl reconfigure-device --mode=system-ram dax0.0 2>&1
echo "exit=$?"
INNER
)
if echo "$OUT4" | grep -q "exit=0"; then
  RESULTS+=("  [PASS] 4. daxctl system-ram reconfigure exit=0")
  PASS=$((PASS+1))
else
  RESULTS+=("  [FAIL] 4. daxctl reconfigure failed")
  RESULTS+=("         $(echo "$OUT4" | tail -3)")
  FAIL=$((FAIL+1))
fi

# ── Check 5: 2 NUMA nodes ────────────────────────────────────
run_check 5 "NUMA shows 2 nodes" \
  'numactl --hardware' \
  "available: 2 nodes"

# ── Summary ───────────────────────────────────────────────────
echo "Results:"
for r in "${RESULTS[@]}"; do echo "$r"; done
echo ""
printf "  %d passed, %d failed\n" "$PASS" "$FAIL"

[ $FAIL -eq 0 ] && echo "ALL CHECKS PASSED" && exit 0
echo "SOME CHECKS FAILED" && exit 1
