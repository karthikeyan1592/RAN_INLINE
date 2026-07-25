#!/bin/bash
# build_tools.sh — build all source-compiled tools on the cxl-poc droplet
#
# Runs AFTER: provision.sh → install_deps.sh → rsync of /root/linux_env/cxl/
# Expected runtime (all):  ~30-40 min
#   bpftime:      ~8 min
#   srsRAN bench: ~15 min
#   CXLMemSim:    ~2 min
#   PoC code:     ~1 min
#
# Usage:
#   ./build_tools.sh                  # build all (except OAI)
#   ./build_tools.sh --with-oai       # also build OAI gNB (~45 min extra)
#   ./build_tools.sh --only bpftime   # build one tool only
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IP_FILE="$SCRIPT_DIR/.cxl_droplet_ip"
[ -f "$IP_FILE" ] || { echo "ERROR: run provision.sh first"; exit 1; }
DROPLET_IP=$(cat "$IP_FILE")
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=30 -o ServerAliveInterval=30"

BUILD_OAI=0
ONLY=""
for arg in "$@"; do
  case "$arg" in
    --with-oai)  BUILD_OAI=1 ;;
    --only)      shift; ONLY="$1" ;;
  esac
done

run_remote() {
  local label="$1"; shift
  echo ""
  echo "════════════════════════════════════════"
  echo "  BUILD: $label"
  echo "════════════════════════════════════════"
  ssh $SSH_OPTS root@"$DROPLET_IP" 'bash -s' "$@"
}

# ─────────────────────────────────────────────────────────────
# 1. bpftime (userspace eBPF — 248 ns uprobe vs 9940 ns kernel)
# ─────────────────────────────────────────────────────────────
build_bpftime() {
run_remote "bpftime" << 'EOF'
set -euo pipefail
echo "[bpftime] cloning / updating..."
if [ -d /root/bpftime ]; then
  cd /root/bpftime && git pull --ff-only 2>/dev/null || true
else
  git clone --recurse-submodules https://github.com/eunomia-bpf/bpftime.git /root/bpftime
fi
cd /root/bpftime

echo "[bpftime] configuring cmake..."
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BPFTIME_DAEMON=OFF \
  -DENABLE_TESTS=OFF \
  -DBPFTIME_ENABLE_VERIFIER=OFF \
  -DBPFTIME_ENABLE_LLVM_JIT=OFF \
  -DBPFTIME_BUILD_STATIC_RUNTIME=OFF \
  -G Ninja 2>&1 | tail -10

echo "[bpftime] building ($(nproc) cores)..."
ninja -j$(nproc) libbpftime-syscall-server.so libbpftime-agent.so bpftool 2>&1 | tail -30

echo ""
echo "[bpftime] artifacts:"
find /root/bpftime/build -name "libbpftime*.so" | sort
find /root/bpftime/build -name "bpftool" -type f | head -3
echo "[bpftime] symlinking .so to /usr/local/lib/ for LD_PRELOAD..."
SERVER_SO=$(find /root/bpftime/build -name "libbpftime-syscall-server.so" | head -1)
AGENT_SO=$(find /root/bpftime/build  -name "libbpftime-agent.so"          | head -1)
[ -n "$SERVER_SO" ] && ln -sf "$SERVER_SO" /usr/local/lib/libbpftime-syscall-server.so
[ -n "$AGENT_SO"  ] && ln -sf "$AGENT_SO"  /usr/local/lib/libbpftime-agent.so
ldconfig
echo "[bpftime] ldconfig done:"
ldconfig -p | grep bpftime || true
echo "[bpftime] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 2. srsRAN_Project — ldpc_decoder_benchmark only
#    (the calibration workload + v6 fallback uprobe target)
# ─────────────────────────────────────────────────────────────
build_srsran() {
run_remote "srsRAN_Project (ldpc_decoder_benchmark)" << 'EOF'
set -euo pipefail
echo "[srsRAN] cloning / updating..."
if [ -d /root/srsRAN_Project ]; then
  cd /root/srsRAN_Project && git pull --ff-only 2>/dev/null || true
else
  git clone --depth=1 --branch main https://github.com/srsran/srsRAN_Project.git /root/srsRAN_Project
fi
cd /root/srsRAN_Project

echo "[srsRAN] configuring cmake (benchmarks enabled)..."
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DENABLE_EXPORT=OFF \
  -DENABLE_UHD=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DENABLE_FFTW=ON \
  -G Ninja 2>&1 | tail -5

echo "[srsRAN] building ldpc_decoder_benchmark (~15 min)..."
ninja -j$(nproc) ldpc_decoder_benchmark 2>&1 | tail -20

BENCH=$(find /root/srsRAN_Project/build -name ldpc_decoder_benchmark -type f | head -1)
echo ""
echo "[srsRAN] benchmark binary: $BENCH"
echo "[srsRAN] smoke test (BG1, LS=384, AVX2, I=5, R=10):"
"$BENCH" -L 384 -I 5 -T avx2 -R 10 2>&1 | grep -E "Mbps|p50|decode" | head -5

# Symlink benchmark binary for easy access
ln -sf "$BENCH" /usr/local/bin/ldpc_decoder_benchmark

# Generate slot-loop wrapper (v6 Gate 1 uprobe target).
# Runs ldpc_decoder_benchmark continuously in ~500 µs slot cadence;
# bpftime attaches to the decode symbol inside this loop.
cat > /usr/local/bin/ldpc_slot_loop.sh << 'LOOP'
#!/bin/bash
# Slot-paced ldpc_decoder_benchmark loop — v6 Gate 1 bpftime uprobe target.
# Emulates 5G NR slot rate: one 500 µs slot per iteration.
# Args: $1=iterations (default 10000), $2=slot_us (default 500)
ITERS="${1:-10000}"
SLOT_US="${2:-500}"
BENCH=/usr/local/bin/ldpc_decoder_benchmark
echo "[slot_loop] start: iters=$ITERS slot_us=${SLOT_US}µs BG1 Z=384 AVX2 I=5"
echo "[slot_loop] PID=$$  — attach bpftime here"
i=0
while [ "$i" -lt "$ITERS" ]; do
  # one CB per slot — the decode call inside is the uprobe target
  "$BENCH" -L 384 -I 5 -T avx2 -R 1 2>/dev/null
  i=$((i+1))
  [ $((i % 500)) -eq 0 ] && echo "[slot_loop] slot $i/$ITERS"
  # sleep to hit ~slot boundary (bench itself takes ~100-200 µs; add the rest)
  sleep_us=$((SLOT_US > 200 ? SLOT_US - 200 : 0))
  [ "$sleep_us" -gt 0 ] && sleep "$(printf '0.%06d' $sleep_us)"
done
echo "[slot_loop] done: $ITERS slots"
LOOP
chmod +x /usr/local/bin/ldpc_slot_loop.sh

echo "[srsRAN] slot loop: $(ls -lh /usr/local/bin/ldpc_slot_loop.sh)"
echo "[srsRAN] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 3. CXLMemSim
# ─────────────────────────────────────────────────────────────
build_cxlmemsim() {
run_remote "CXLMemSim" << 'EOF'
set -euo pipefail
CXLSIM=/root/cxl/third_party/CXLMemSim
[ -d "$CXLSIM" ] || { echo "ERROR: $CXLSIM not found — did rsync run?"; exit 1; }
cd "$CXLSIM"

echo "[CXLMemSim] configuring cmake..."
rm -rf build && mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SLUGALLOCATOR=OFF \
  -G Ninja 2>&1 | tail -5

echo "[CXLMemSim] building..."
ninja -j$(nproc) 2>&1 | tail -10

echo ""
echo "[CXLMemSim] artifacts:"
ls -lh cxlmemsim_legacy cxlmemsim_latency 2>/dev/null || ls -lh cxlmemsim* 2>/dev/null
echo "[CXLMemSim] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 4. CXL PoC source (our code: phase5_cxl, gpu_daemon, etc.)
# ─────────────────────────────────────────────────────────────
build_poc() {
run_remote "CXL PoC source (phase5_cxl + gpu_daemon)" << 'EOF'
set -euo pipefail
POC=/root/cxl/cxl_ran_poc
[ -d "$POC" ] || { echo "ERROR: $POC not found — did rsync run?"; exit 1; }

BPFTIME_DIR=/root/bpftime

# Check bpftime is built
LIBBPF_A=$(find "$BPFTIME_DIR/build" -name libbpf.a 2>/dev/null | head -1)
BPFTOOL=$(find "$BPFTIME_DIR/build" -name bpftool -type f 2>/dev/null | head -1 || \
          command -v bpftool 2>/dev/null || echo "")
if [ -z "$LIBBPF_A" ]; then
  echo "WARNING: bpftime libbpf.a not found; using system libbpf"
  BPFTIME_MAKE_ARGS=""
else
  BPFTIME_MAKE_ARGS="BPFTIME_DIR=$BPFTIME_DIR LIBBPF_A=$LIBBPF_A"
  [ -n "$BPFTOOL" ] && BPFTIME_MAKE_ARGS="$BPFTIME_MAKE_ARGS BPFTOOL=$BPFTOOL"
fi

echo "[poc] building phase5_cxl (all phases)..."
cd "$POC/phase5_cxl"
make clean 2>/dev/null || true
# phase1: ring_test consumer (SPSC ring sanity)
# phase2: + consumer_ocl cxl_bit_diff (bit-correctness)
# phase3: + ldpc_consumer_v5 (the v6 uprobe consumer, requires bpf skeleton)
# phase4: + ablation ocl_bench_standalone (measurement)
make phase1 $BPFTIME_MAKE_ARGS 2>&1 | tail -5
# phase2 (consumer_ocl, cxl_bit_diff) skipped — source files not implemented
make phase3 $BPFTIME_MAKE_ARGS 2>&1 | tail -5
make phase4 $BPFTIME_MAKE_ARGS 2>&1 | tail -5
echo "  built: $(ls -1 ring_test consumer ldpc_consumer_v5 ablation ocl_bench_standalone 2>/dev/null | tr '\n' ' ')"

echo "[poc] building gpu_daemon..."
cd "$POC/gpu_daemon"
make clean 2>/dev/null || true
make 2>&1
echo "  built: $(ls -1 gpu_daemon 2>/dev/null)"

echo "[poc] building l1_sim..."
cd "$POC/l1_sim"
make clean 2>/dev/null || true
make 2>&1
echo "  built: $(ls -1 ran_l1_sim 2>/dev/null)"

echo "[poc] DONE"
EOF
}

# ─────────────────────────────────────────────────────────────
# 5. OAI gNB (OPTIONAL — ~45 min)
# ─────────────────────────────────────────────────────────────
build_oai() {
run_remote "OAI gNB (--gNB --nrUE -w SIMU)" << 'EOF'
set -euo pipefail
OAI=/root/cxl/third_party/openairinterface5g
[ -d "$OAI" ] || {
  echo "[OAI] not in rsync tree; cloning fresh..."
  git clone --depth=1 https://gitlab.eurecom.fr/oai/openairinterface5g.git /root/oai
  OAI=/root/oai
}
cd "$OAI"

echo "[OAI] running build_oai --gNB --nrUE -w SIMU..."
echo "[OAI] this takes ~45 minutes..."
cd cmake_targets
./build_oai --gNB --nrUE -w SIMU -I 2>&1 | tee /tmp/oai_build.log | tail -30

echo ""
echo "[OAI] checking for gNB binary..."
find "$OAI" -name "nr-softmodem" -type f 2>/dev/null | head -3
echo "[OAI] checking for libldpc.so (uprobe target)..."
find "$OAI" -name "libldpc*.so" -o -name "libldpc*.a" 2>/dev/null | head -5
echo "[OAI] DONE — full log at /tmp/oai_build.log"
EOF
}

# ─────────────────────────────────────────────────────────────
# Final verification — confirm all key artifacts exist
# ─────────────────────────────────────────────────────────────
verify_builds() {
run_remote "Artifact verification" << 'EOF'
ok()  { printf "  [OK]  %-45s %s\n" "$1" "${2:-}"; }
fail(){ printf "  [!!]  %-45s MISSING\n" "$1"; FAIL=1; }
FAIL=0

# bpftime
f=/root/bpftime/build/runtime/syscall-server/libbpftime-syscall-server.so
[ -f "$f" ] && ok "bpftime: libbpftime-syscall-server.so" "$(ls -sh $f | awk '{print $1}')" || fail "bpftime: libbpftime-syscall-server.so"
f=/root/bpftime/build/runtime/agent/libbpftime-agent.so
[ -f "$f" ] && ok "bpftime: libbpftime-agent.so" "$(ls -sh $f | awk '{print $1}')" || fail "bpftime: libbpftime-agent.so"
f=/root/bpftime/build/bpftool/bpftool
[ -f "$f" ] && ok "bpftime: bpftool" "" || fail "bpftime: bpftool"
# ldconfig entries
ldconfig -p | grep -q "libbpftime-syscall-server" && \
  ok "ldconfig: libbpftime-syscall-server (LD_PRELOAD ready)" "" || fail "ldconfig: libbpftime-syscall-server"

# srsRAN
f=$(find /root/srsRAN_Project/build -name ldpc_decoder_benchmark -type f 2>/dev/null | head -1)
[ -n "$f" ] && ok "srsRAN: ldpc_decoder_benchmark" "$(ls -sh $f | awk '{print $1}')" || fail "srsRAN: ldpc_decoder_benchmark"
[ -L /usr/local/bin/ldpc_decoder_benchmark ] && ok "srsRAN: /usr/local/bin/ symlink" "" || fail "srsRAN: /usr/local/bin/ldpc_decoder_benchmark symlink"
[ -x /usr/local/bin/ldpc_slot_loop.sh ] && ok "srsRAN: ldpc_slot_loop.sh (Gate 1 uprobe target)" "" || fail "srsRAN: ldpc_slot_loop.sh"

# CXLMemSim
f=/root/cxl/third_party/CXLMemSim/build/cxlmemsim_legacy
[ -f "$f" ] && ok "CXLMemSim: cxlmemsim_legacy" "$(ls -sh $f | awk '{print $1}')" || fail "CXLMemSim: cxlmemsim_legacy"

# PoC phase binaries
POC=/root/cxl/cxl_ran_poc/phase5_cxl
for b in ring_test consumer ldpc_consumer_v5 ablation ocl_bench_standalone; do
  [ -f "$POC/$b" ] && ok "poc/phase5_cxl: $b" "" || fail "poc/phase5_cxl: $b"
done
f=/root/cxl/cxl_ran_poc/gpu_daemon/gpu_daemon
[ -f "$f" ] && ok "poc/gpu_daemon: gpu_daemon" "" || fail "poc/gpu_daemon: gpu_daemon"
f=/root/cxl/cxl_ran_poc/l1_sim/ran_l1_sim
[ -f "$f" ] && ok "poc/l1_sim: ran_l1_sim" "" || fail "poc/l1_sim: ran_l1_sim"

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "  All artifacts present — ready for prepare_vm.sh"
else
  echo "  Some artifacts missing — re-run the failing target"
  exit 1
fi
EOF
}

# ─────────────────────────────────────────────────────────────
# Dispatch
# ─────────────────────────────────────────────────────────────
if [ -n "$ONLY" ]; then
  case "$ONLY" in
    bpftime)   build_bpftime ;;
    srsran)    build_srsran ;;
    cxlmemsim) build_cxlmemsim ;;
    poc)       build_poc ;;
    oai)       build_oai ;;
    *) echo "Unknown target: $ONLY. Choose: bpftime srsran cxlmemsim poc oai"; exit 1 ;;
  esac
else
  build_bpftime
  build_srsran
  build_cxlmemsim
  build_poc
  [ "$BUILD_OAI" -eq 1 ] && build_oai || echo ""
fi

if [ -z "$ONLY" ]; then
  verify_builds
fi

echo ""
echo "════════════════════════════════════════"
echo "  build_tools.sh complete"
[ "$BUILD_OAI" -eq 0 ] && echo "  OAI skipped — run with --with-oai to build gNB"
echo "  Next: ./prepare_vm.sh"
echo "════════════════════════════════════════"
