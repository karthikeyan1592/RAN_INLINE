#!/bin/bash
# build_tools.sh — build all source-compiled tools on GCP CXL lab instance
#
# Run after: provision.sh → install_deps.sh → rsync_source.sh
# Expected runtime:  bpftime ~10 min  |  srsRAN bench ~15 min  |  PoC ~1 min
#
# Usage:
#   ./build_tools.sh                  # build all
#   ./build_tools.sh --only bpftime   # single target
#   ./build_tools.sh --only srsran
#   ./build_tools.sh --only poc
#   ./build_tools.sh --only cxlmemsim
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.gcp_instance_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first (.gcp_instance_ip missing)"; exit 1; }
GCP_IP=$(cat "$IP_FILE")
GCP_USER="${GCP_USER:-karthix25}"
GCP_KEY="${GCP_KEY:-$HOME/.ssh/id_ed25519}"
SSH_OPTS="-i $GCP_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=30 -o ServerAliveInterval=30"

ONLY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --only) ONLY="${2:-}"; shift 2 ;;
    *) shift ;;
  esac
done

run_remote() {
  local label="$1"; shift
  echo ""
  echo "════════════════════════════════════════"
  echo "  BUILD: $label"
  echo "════════════════════════════════════════"
  ssh $SSH_OPTS "${GCP_USER}@${GCP_IP}" 'bash -s' "$@"
}

# ─────────────────────────────────────────────────────────────
# 1. bpftime — with DEV-035 cmake patch (bpftool symlinks point
#    to /root/linux_env which does not exist on GCP)
# ─────────────────────────────────────────────────────────────
build_bpftime() {
run_remote "bpftime (with DEV-035 cmake patch)" << 'EOF'
set -euo pipefail
BPFTIME=/home/karthix25/cxl/third_party/bpftime

echo "[bpftime] source: $BPFTIME"
[ -d "$BPFTIME" ] || { echo "ERROR: bpftime not found — did rsync_source.sh run?"; exit 1; }
cd "$BPFTIME"

# DEV-035: bpftool submodule has absolute symlinks to /root/linux_env.
# Replace ExternalProject_Add(bpftool) with find_program() pointing at system bpftool.
echo "[bpftime] applying DEV-035 cmake patch..."
python3 - << 'PYEOF'
import re, sys
path = "cmake/libbpf.cmake"
with open(path) as f:
    txt = f.read()

# Already patched?
if "find_program(BPFTOOL_BINARY" in txt:
    print("  already patched — skipping")
    sys.exit(0)

pattern = re.compile(
    r'ExternalProject_Add\s*\(\s*bpftool\b.*?^\)',
    re.DOTALL | re.MULTILINE
)
m = pattern.search(txt)
if not m:
    # Try a looser match for the whole block up to the closing paren on its own line
    pattern = re.compile(
        r'ExternalProject_Add\s*\(\s*bpftool\b.*?set\s*\(\s*BPFTOOL_INSTALL_DIR[^\)]*\)',
        re.DOTALL
    )
    m = pattern.search(txt)

if not m:
    print("  WARNING: ExternalProject_Add(bpftool) block not found")
    print("  Check cmake/libbpf.cmake manually for the bpftool block")
    sys.exit(0)

replacement = (
    "find_program(BPFTOOL_BINARY bpftool REQUIRED)\n"
    "set(BPFTOOL_INSTALL_DIR /usr/sbin)\n"
    "add_custom_target(bpftool)  # no-op so dependents build"
)
txt = txt[:m.start()] + replacement + txt[m.end():]
with open(path, "w") as f:
    f.write(txt)
print("  DEV-035 patch applied to cmake/libbpf.cmake")
PYEOF

echo "[bpftime] configuring cmake..."
rm -rf build && mkdir build
cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BPFTIME_DAEMON=OFF \
  -DENABLE_TESTS=OFF \
  -DBPFTIME_ENABLE_VERIFIER=OFF \
  -DBPFTIME_LLVM_JIT=OFF \
  -DBPFTIME_BUILD_STATIC_RUNTIME=OFF \
  2>&1 | tail -8

echo "[bpftime] building ($(nproc) cores)..."
make -j$(nproc) 2>&1 | tail -20

echo ""
echo "[bpftime] artifacts:"
find /home/karthix25/cxl/third_party/bpftime/build -name "libbpftime*.so" 2>/dev/null | sort

# Symlink .so files for LD_PRELOAD use
SERVER_SO=$(find /home/karthix25/cxl/third_party/bpftime/build \
  -name "libbpftime-syscall-server.so" 2>/dev/null | head -1)
AGENT_SO=$(find /home/karthix25/cxl/third_party/bpftime/build \
  -name "libbpftime-agent.so" 2>/dev/null | head -1)
[ -n "$SERVER_SO" ] && sudo ln -sf "$SERVER_SO" /usr/local/lib/libbpftime-syscall-server.so
[ -n "$AGENT_SO"  ] && sudo ln -sf "$AGENT_SO"  /usr/local/lib/libbpftime-agent.so
sudo ldconfig

echo "[bpftime] uprobe offset (for Gate 1/2 reference):"
BENCH=$(find /home/karthix25/cxl/third_party/srsRAN_Project/build \
  -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1)
if [ -n "$BENCH" ]; then
  nm -C "$BENCH" 2>/dev/null \
    | grep -i "ldpc_decoder_impl::decode" | grep " T " \
    | awk '{printf "  0x%s  %s\n", $1, $3}' | head -3 || true
else
  echo "  (srsRAN bench not built yet — run --only srsran first)"
fi
echo "[bpftime] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 2. srsRAN_Project — ldpc_decoder_benchmark
#    Primary calibration workload; also the Gate 1/2 uprobe target.
#    Per PRIMARY_CONFIG: BG1 Z=384 AVX2 I=20 → 487.6 µs/CB = 11703 µs/slot (24 CB).
# ─────────────────────────────────────────────────────────────
build_srsran() {
run_remote "srsRAN_Project (ldpc_decoder_benchmark)" << 'EOF'
set -euo pipefail
SRSRAN=/home/karthix25/cxl/third_party/srsRAN_Project
[ -d "$SRSRAN" ] || { echo "ERROR: srsRAN not found — did rsync_source.sh run?"; exit 1; }
cd "$SRSRAN"

echo "[srsRAN] configuring cmake (benchmark target only)..."
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DENABLE_EXPORT=ON \
  -DENABLE_UHD=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DENABLE_FFTW=ON \
  2>&1 | tail -5

echo "[srsRAN] building ldpc_decoder_benchmark (~15 min)..."
make -j$(nproc) ldpc_decoder_benchmark 2>&1 | tail -20

BENCH=$(find "$SRSRAN/build" -name ldpc_decoder_benchmark -type f | head -1)
echo ""
echo "[srsRAN] binary: $BENCH ($(ls -sh $BENCH | awk '{print $1}'))"

echo "[srsRAN] smoke test (BG1, Z=384, AVX2, I=5, R=10):"
"$BENCH" -L 384 -I 5 -T avx2 -R 10 2>&1 | grep -E "Mbps|throughput|decode" | head -5

sudo ln -sf "$BENCH" /usr/local/bin/ldpc_decoder_benchmark
echo "[srsRAN] symlinked → /usr/local/bin/ldpc_decoder_benchmark"

# Derive and record uprobe offset for THIS build.
# Use nm -C (demangle) so word boundary \b works after "decode" before "(".
# The || true prevents pipefail from exiting if grep finds no match.
OFFSET=$(nm -C "$BENCH" 2>/dev/null \
  | grep -i "ldpc_decoder_impl::decode" \
  | grep " T " \
  | awk '{print $1}' | head -1 || true)
if [ -n "$OFFSET" ]; then
  echo "UPROBE_OFFSET=0x${OFFSET}" | sudo tee /etc/cxl_poc_uprobe_offset
  echo "[srsRAN] uprobe offset: 0x${OFFSET} (saved to /etc/cxl_poc_uprobe_offset)"
else
  echo "[srsRAN] WARNING: could not derive uprobe offset — set /etc/cxl_poc_uprobe_offset manually"
fi
echo "[srsRAN] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 3. CXL PoC gate programs (gate0_option_a, gate2_xproc)
#    These run INSIDE the QEMU VM. Build here on GCP host too
#    so the source is compiled and any build errors surface early.
# ─────────────────────────────────────────────────────────────
build_poc() {
run_remote "CXL PoC gate programs (phase5_cxl)" << 'EOF'
set -euo pipefail
POC=/home/karthix25/cxl/cxl_ran_poc
[ -d "$POC" ] || { echo "ERROR: $POC not found — did rsync_source.sh run?"; exit 1; }

echo "[poc] building phase5_cxl gate programs..."
cd "$POC/phase5_cxl"

# gate0_option_a: CXL zero-copy NUMA check (Gate 0)
if [ -f gate0_option_a.c ]; then
  gcc -O2 -Wall gate0_option_a.c -lnuma -lOpenCL -o gate0_option_a \
    2>&1 && echo "  gate0_option_a: OK" || echo "  gate0_option_a: FAIL (see above)"
fi

# gate2_xproc: cross-process LLR extraction (Gate 2)
if [ -f gate2_xproc.c ]; then
  gcc -O2 -Wall gate2_xproc.c -lnuma -lOpenCL -lpthread -o gate2_xproc \
    2>&1 && echo "  gate2_xproc: OK" || echo "  gate2_xproc: FAIL (see above)"
fi

echo ""
echo "[poc] built:"
ls -lh gate0_option_a gate2_xproc ablation ocl_bench_standalone 2>/dev/null || true

echo "[poc] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 4. CXLMemSim (optional, for paper comparison tables)
# ─────────────────────────────────────────────────────────────
build_cxlmemsim() {
run_remote "CXLMemSim" << 'EOF'
set -euo pipefail
CXLSIM=/home/karthix25/cxl/third_party/CXLMemSim
[ -d "$CXLSIM" ] || { echo "ERROR: $CXLSIM not found — did rsync_source.sh run?"; exit 1; }
cd "$CXLSIM"

echo "[CXLMemSim] configuring cmake..."
rm -rf build && mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SLUGALLOCATOR=OFF \
  2>&1 | tail -4

echo "[CXLMemSim] building..."
make -j$(nproc) 2>&1 | tail -8

echo "[CXLMemSim] built: $(ls -lh cxlmemsim* 2>/dev/null | awk '{print $5, $9}' | tr '\n' '  ')"
echo "[CXLMemSim] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# Final: artifact verification
# ─────────────────────────────────────────────────────────────
verify_builds() {
run_remote "Artifact verification" << 'EOF'
ok()  { printf "  [OK]  %-50s %s\n" "$1" "${2:-}"; }
fail(){ printf "  [!!]  %-50s MISSING\n" "$1"; FAIL=1; }
FAIL=0

BASE=/home/karthix25/cxl

# bpftime
for so in libbpftime-syscall-server.so libbpftime-agent.so; do
  f=$(find "$BASE/third_party/bpftime/build" -name "$so" 2>/dev/null | head -1)
  [ -n "$f" ] && ok "bpftime: $so" "$(ls -sh $f | awk '{print $1}')" || fail "bpftime: $so"
done

# srsRAN
f=$(find "$BASE/third_party/srsRAN_Project/build" -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1)
[ -n "$f" ] && ok "srsRAN: ldpc_decoder_benchmark" "$(ls -sh $f | awk '{print $1}')" || fail "srsRAN: ldpc_decoder_benchmark"
[ -L /usr/local/bin/ldpc_decoder_benchmark ] && ok "srsRAN: /usr/local/bin symlink" "" || fail "srsRAN: /usr/local/bin/ldpc_decoder_benchmark symlink"
[ -f /etc/cxl_poc_uprobe_offset ] && ok "uprobe offset saved" "$(cat /etc/cxl_poc_uprobe_offset)" || fail "uprobe offset (/etc/cxl_poc_uprobe_offset)"

# PoC gate programs
POC="$BASE/cxl_ran_poc/phase5_cxl"
for b in gate0_option_a gate2_xproc; do
  [ -f "$POC/$b" ] && ok "poc/phase5_cxl: $b" "" || fail "poc/phase5_cxl: $b"
done

# CXLMemSim (optional)
f=$(find "$BASE/third_party/CXLMemSim/build" -name "cxlmemsim*" -type f 2>/dev/null | head -1)
[ -n "$f" ] && ok "CXLMemSim: $(basename $f)" "$(ls -sh $f | awk '{print $1}')" || echo "  [--]  CXLMemSim: not built (optional)"

echo ""
[ "$FAIL" -eq 0 ] && echo "  All required artifacts present" || { echo "  Some artifacts missing — re-run the failing target"; exit 1; }
EOF
}

# ─────────────────────────────────────────────────────────────
# Dispatch
# ─────────────────────────────────────────────────────────────
case "${ONLY:-}" in
  bpftime)    build_bpftime ;;
  srsran)     build_srsran ;;
  poc)        build_poc ;;
  cxlmemsim)  build_cxlmemsim ;;
  "")
    build_srsran
    build_bpftime
    build_poc
    build_cxlmemsim
    verify_builds
    ;;
  *) echo "Unknown target: $ONLY. Choose: bpftime srsran poc cxlmemsim"; exit 1 ;;
esac

echo ""
echo "════════════════════════════════════════"
echo "  build_tools.sh complete"
echo "  Next: ./prepare_vm.sh → ./launch_vm.sh → ./run_e2e_test.sh"
echo "════════════════════════════════════════"
