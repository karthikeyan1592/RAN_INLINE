# Gate 0.2 — OAI build + RFsim transport + netns/veth setup

## Spec (verbatim from cursor_cxl_poc_prompt_v4.md)

```
## 0.2 — OAI build + RFsim transport + netns/veth setup

git clone https://github.com/OPENAIRINTERFACE/openairinterface5g.git
cd openairinterface5g
./build_oai --gNB --nrUE -w SIMU 2>&1 | tee build_oai.log

# Confirm the LDPC interception target exists:
nm -D cmake_targets/ran_build/build/liboai_device.so 2>/dev/null \
  | grep -i ldpc
find . -name "libldpc*.so" -exec nm -D {} \; 2>/dev/null | grep -i ldpc
# Expect: nrLDPC_coding_decoder and/or nrLDPC_coding_segment_decoder

Regardless of what transport OAI's rfsimulator uses by default, build
the netns/veth setup so the RFsim link is UNAMBIGUOUSLY real
network-stack traffic:

# Two network namespaces, veth pair between them
ip netns add ue-ns
ip netns add gnb-ns
ip link add veth-ue type veth peer name veth-gnb
ip link set veth-ue netns ue-ns
ip link set veth-gnb netns gnb-ns
ip netns exec ue-ns  ip addr add 10.77.0.1/24 dev veth-ue
ip netns exec gnb-ns ip addr add 10.77.0.2/24 dev veth-gnb
ip netns exec ue-ns  ip link set veth-ue up
ip netns exec gnb-ns ip link set veth-gnb up
ip netns exec ue-ns  ip link set lo up
ip netns exec gnb-ns ip link set lo up

### GATE 0.2
PASS if: both netns exist, veth pair up, gNB --rfsim --phy-test
         launched with --rfsimulator.serveraddr 10.77.0.2 (in gnb-ns)
         and UE with --rfsimulator.serveraddr 10.77.0.2 (in ue-ns)
         establishes a connection (check OAI logs for "rfsimulator"
         connect messages on both sides).

FAIL  -> debug netns/veth networking first (this is standard Linux
         networking, not exotic — a ping between 10.77.0.1 and
         10.77.0.2 across the veth pair is the minimal check before
         even involving OAI).
```

## Commands run

```bash
# 2026-06-15  OAI build (done in prior session, evidence below)
# OAI_BUILD=/root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build

# LDPC symbol confirmation
# 2026-06-15 08:45
nm -D $OAI_BUILD/libldpc.so | grep -i ldpc

# netns/veth setup (done in prior session, verified persisted)
ip netns list
ip netns exec gnb-ns ip addr show veth-gnb
ip netns exec ue-ns  ip addr show veth-ue

# gNB launch in gnb-ns (2026-06-15T08:50:00Z)
ip netns exec gnb-ns bash -c "
  cd /tmp/oai_gnb
  LD_LIBRARY_PATH=$OAI_BUILD $OAI_BUILD/nr-softmodem \
    -O $OAI_CONF_DIR/gnb.band66.106prb.rfsim.phytest-dora.conf \
    --phy-test --rfsim --noS1 \
    '--rfsimulator.[0].wait_timeout' 20 \
    --log_config.global_log_level info \
    2>&1
" > /tmp/oai_gnb/gnb.log 2>&1 &

# Confirmed port 4043 listening in gnb-ns:
ip netns exec gnb-ns ss -tlnp | grep 4043

# UE launch in ue-ns (2026-06-15T08:51:00Z)
ip netns exec ue-ns bash -c "
  cd /tmp/oai_ue
  LD_LIBRARY_PATH=$OAI_BUILD $OAI_BUILD/nr-uesoftmodem \
    -O $OAI_CONF_DIR/nrue.uicc.conf \
    --phy-test --rfsim --noS1 \
    '--rfsimulator.[0].serveraddr' 10.77.0.2 \
    --reconfig-file /tmp/oai_ue/reconfig.raw \
    --rbconfig-file /tmp/oai_ue/rbconfig.raw \
    --log_config.global_log_level info \
    2>&1
" > /tmp/oai_ue/ue.log 2>&1 &

# Confirmed TCP ESTAB in both netns (2026-06-15T08:58:23Z):
ip netns exec gnb-ns ss -tnp | grep 4043
ip netns exec ue-ns  ss -tnp | grep 4043
```

## Raw evidence

### OAI build artifacts
```
ls $OAI_BUILD/ | grep -E 'nr-soft|nr-ue|libldpc|reconfig|rbconfig'
nr-softmodem
nr-uesoftmodem
libldpc.so
libldpc_orig.so
rbconfig.raw
reconfig.raw
```

### LDPC symbol table (full relevant output)
```
nm -D $OAI_BUILD/libldpc.so | grep -i ldpc
0000000000063110 T LDPCdecoder
00000000000dec90 T LDPCencoder
00000000000630f0 T LDPCinit
0000000000063100 T LDPCshutdown
00000000000e9b30 T nrLDPC_coding_decoder
00000000000eb220 T nrLDPC_coding_encoder
00000000000e9b10 T nrLDPC_coding_init
00000000000e9b20 T nrLDPC_coding_shutdown
00000000000e9380 T nrLDPC_prepare_TB_decoding
00000000000e6880 T nr_deinterleaving_ldpc
00000000000e5ef0 T nr_interleaving_ldpc
00000000000e74c0 T nr_rate_matching_ldpc
00000000000e7bb0 T nr_rate_matching_ldpc_rx
```
Phase 2 bpftime attach point: `nrLDPC_coding_decoder` @ `0x000e9b30` in `libldpc.so`.
(Note: `nrLDPC_coding_segment_decoder` is NOT present — per-slot function is the attach point.)

### netns/veth verification
```
ip netns list
gnb-ns (id: 1)
ue-ns (id: 0)

ip netns exec gnb-ns ip addr show veth-gnb
4057: veth-gnb@if4058: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP
    link/ether 92:c6:ec:e5:e1:22 brd ff:ff:ff:ff:ff:ff link-netns ue-ns
    inet 10.77.0.2/24 scope global veth-gnb

ip netns exec ue-ns ip addr show veth-ue
4058: veth-ue@if4057: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP
    link/ether fe:80:dd:2f:a8:10 brd ff:ff:ff:ff:ff:ff link-netns gnb-ns
    inet 10.77.0.1/24 scope global veth-ue
```

### Port 4043 listening (gnb-ns)
```
ip netns exec gnb-ns ss -tlnp | grep 4043
LISTEN 0      5                  *:4043            *:*    users:(("nr-softmodem",pid=985740,fd=82))
```

### gNB rfsimulator connect message
```
[HW]     Running as server waiting opposite rfsimulators to connect
[HW]     Client connects from ::ffff:10.77.0.1:60050
```

### UE rfsimulator connect messages
```
[HW]     Running as client: will connect to a rfsimulator server side
[HW]     Trying to connect to 10.77.0.2:4043
[HW]     Connection to 10.77.0.2:4043 established
```

### TCP ESTAB in both netns (real network stack — not AF_UNIX)
```
ip netns exec gnb-ns ss -tnp | grep 4043
ESTAB 430528 368704 [::ffff:10.77.0.2]:4043 [::ffff:10.77.0.1]:37610 users:(("nr-softmodem",pid=985740,fd=84))

ip netns exec ue-ns ss -tnp | grep 4043
ESTAB 245824 32         10.77.0.1:37610    10.77.0.2:4043 users:(("nr-uesoftmodem",pid=985900,fd=81))
```

### gNB MAC status (LDPC encoder active, DLSCH rounds running)
```
UE RNTI 1234 CU-UE-ID 1 in-sync PH 0 dB PCMAX 0 dBm
UE 1234: dlsch_rounds 11232/11232/11232/11231, dlsch_errors 11231, pucch0_DTX 44927 (SNR 14.0-1.0 dB), BLER 0.10000 MCS (0) 9 CCE fail 0, goodput 0.00 Mbps
UE 1234: ulsch_rounds 11232/11232/11232/11231, ulsch_errors 11231, ulsch_DTX 44927, BLER 0.10000 MCS (0) 9 (Qm 2 deltaMCS 0 dB) NPRB 50 SNR -5.0 (-25.0) dB CCE fail 0, goodput 0.00 Mbps
```
(Note: errors are expected in phy-test mode — the UE PHY sync fails due to config mismatch between
the pre-built reconfig.raw and the phytest-dora gNB config, causing NACK feedback. The LDPC encoder
on the gNB side is still running; see DEV-007 for details. This does not affect Gate 0.2 pass criteria.)

## Self-reported verdict

**PASS** — All Gate 0.2 criteria met:

1. ✓ Both netns exist: `gnb-ns (id: 1)`, `ue-ns (id: 0)`
2. ✓ Veth pair up: `veth-gnb@10.77.0.2 UP` in gnb-ns, `veth-ue@10.77.0.1 UP` in ue-ns
3. ✓ gNB `--phy-test --rfsim --noS1` launched in gnb-ns; server listens on `*:4043`
4. ✓ UE `--phy-test --rfsim --noS1 --rfsimulator.[0].serveraddr 10.77.0.2` launched in ue-ns
5. ✓ rfsimulator connect messages on **both sides**:
   - gNB: `[HW] Client connects from ::ffff:10.77.0.1:60050`
   - UE: `[HW] Connection to 10.77.0.2:4043 established`
6. ✓ TCP ESTAB confirmed in both netns — real network stack (not AF_UNIX)
7. ✓ LDPC attach point confirmed: `nrLDPC_coding_decoder @ 0x000e9b30` in `libldpc.so`

Transport type: **TCP** (not AF_UNIX). The RFsim IQ samples travel over the real Linux
network stack through the veth pair — this is Band 1 of the CXL-RAN architecture.

## Deviations from spec

- DEV-006: Spec says `--rfsimulator.serveraddr 10.77.0.2` for gNB. OAI nr-softmodem
  does not accept this flag (`[CONFIG] unknown option: --rfsimulator.serveraddr`). Omitted
  for gNB; gNB binds on all interfaces inside gnb-ns (only interface is 10.77.0.2). Effect
  is equivalent — confirmed by `ss` showing ESTAB on 10.77.0.2:4043.
- DEV-007: Informational. UE PHY sync fails (synch Failed) during the test run because
  the pre-built `reconfig.raw`/`rbconfig.raw` in the build directory were generated for
  different cell parameters than the phytest-dora config. This causes HARQ NACKs (BLER 1.0)
  but does NOT affect Gate 0.2 pass criteria (which requires only rfsimulator connect messages).
  The gNB LDPC encoder is running (11232 dlsch_rounds observed). Phase 2 will resolve by
  letting the gNB generate fresh raw files on startup before the UE connects.

## Files produced/modified

- third_party/openairinterface5g/ (already built from prior session)
- memory/v4_run/implementer/phase0/gate_0.2_netns.md (this file)
- /tmp/oai_gnb/gnb.log (gNB log, not committed)
- /tmp/oai_ue/ue.log (UE log, not committed)

## Timestamp

2026-06-15T09:00:00Z
