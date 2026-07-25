# OAI (OpenAirInterface) build notes — v4 session

## Source

```bash
git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git \
    /root/linux_env/cxl/third_party/openairinterface5g --depth=1
cd /root/linux_env/cxl/third_party/openairinterface5g
```

## Build (phytest targets only)

```bash
cd cmake_targets
./build_oai -c -C         # clean
./build_oai --gNB --nrUE \
    -t Ethernet \
    --build-lib "ldpc" \
    --ninja
# Output: ran_build/build/nr-softmodem
#         ran_build/build/nr-uesoftmodem
#         ran_build/build/libldpc.so
```

## Key uprobe offsets (verify per build)

```bash
nm -D ran_build/build/libldpc.so | grep -E "nrLDPC_coding_decoder|LDPCdecoder"
# v4 session values:
#   nrLDPC_coding_decoder  0x000e9b30  T
#   LDPCdecoder            0x00063110  T
```

These change with each build. Always verify before running bpftime probes.

## Configs used in v4

| Config | Purpose |
|--------|---------|
| `ci-scripts/conf_files/gnb.band66.106prb.rfsim.phytest-dora.conf` | gNB phytest (C_actual=2) |
| `ci-scripts/conf_files/nrue.uicc.conf` | UE for rfsimulator |

## Critical runtime notes

### reconfig.raw / rbconfig.raw

The gNB writes `reconfig.raw` and `rbconfig.raw` to its **current working directory**
at startup. The UE must read from the same path. Mismatch → UE PHY sync failure (DEV-007).

```bash
# gNB: cd to a known dir so reconfig.raw lands there
ip netns exec gnb-ns bash -c "cd /tmp/oai_run && \
  LD_PRELOAD=.../libbpftime-agent.so \
  .../nr-softmodem -O gnb.conf --phy-test --rfsim --noS1 ..."

# UE: point at same dir
ip netns exec ue-ns .../nr-uesoftmodem \
    --reconfig-file /tmp/oai_run/reconfig.raw \
    --rbconfig-file /tmp/oai_run/rbconfig.raw ...
```

### gNB --rfsimulator.serveraddr unsupported

`--rfsimulator.serveraddr` is not a valid command-line flag (DEV-006). The gNB
binds to all interfaces in its network namespace. Inside gnb-ns with only
`veth-gnb@10.77.0.2`, it effectively binds on 10.77.0.2. No flag needed.

### UE sync takes ~15s on first connect

With the correct reconfig.raw, UE PHY sync takes 10–15 seconds. Probes with
a <15s no-event timeout will fire before LDPC decoding starts. Use 45s timeout.

### Threading: C_actual=2 not C=24

`nrLDPC_coding_segment_decoder.c:281` calls `pushTpool` per CB — concurrent
`LDPCdecoder` calls from thread pool. With phy-test config: C_actual=2 CB/slot.
Use `BPF_MAP_TYPE_PERCPU_ARRAY` for any BPF counter to avoid atomic-opcode issues.
