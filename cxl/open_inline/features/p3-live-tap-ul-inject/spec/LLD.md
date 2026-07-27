# p3-live-tap-ul-inject — LLD

> Pins: OCUDU `release_26_04`, `apps/examples/ofh/`, namespace `ocudu`. Patch point verified against
> `third_party/ocudu` (local clone). MVP config values (numerology, PRB, MCS, eAxC) per
> `p2-phy-kernels` SPEC.md "Fixed MVP configuration" table, shared across P1/P2/P3 by design.

## Module breakdown

| Module | Location (post-implementation) | Kind | Depends on |
|---|---|---|---|
| M1 — ru_emulator oracle-injection patch | patch series against `apps/examples/ofh/ru_emulator.cpp` (+ `ru_emulator_appconfig.h`, `ru_emulator_cli11_schema.cpp`) | C++ patch, upstream file | OCUDU `release_26_04` source |
| M2 — Oracle-grid loader | new files added by the patch, e.g. `ru_emulator_oracle_grid.{h,cpp}`, linked into the `ru-emulator` binary | C++, in-process with M1 | file format §3.1 |
| M3 — gpu-phy af_packet tap (`ingest_backend` SIM impl) + O-RAN header pre-parse | new source in `gpu-phy` image, implements the `oi_ingest` API shape | C++ | `oi_p2_feed`/`oi_frame_desc` (p2-phy-kernels LLD §4.1, concrete — see note below) |
| M4 — pcap byte-comparator | host tool | script/binary | oracle file format §3.1, pcap corpus |
| M5 — live bit-exact harness | host tool | script/binary, drains `oi_p2_drain` | p2 opaque drain API, oracle file format |
| M6 — DU-undisturbed checker | host tool | reuses P1's KPI/log-diff scripts unmodified | P1 LLD Error handling / KPI snapshot |

M1/M2 run **inside** the `ru-emu` container. M3 runs inside `gpu-phy` (linked with p2's pipeline
library). M4/M5/M6 are host-side, matching P1's placement convention (never inside a rig container).

## Public APIs

### M2 — Oracle-grid loader (patch-internal; no external ABI, declared here for review/testability)

```
// Loaded once at ru_emulator startup, from the config's oracle_injection.files list (LLD §4).
struct oracle_grid_set {
  std::vector<oracle_grid_entry> entries;   // ordered, index = schedule position (P3-R6)
  ru_compression_params          iq_format; // parsed from entries[0], validated equal across all entries
};

// One loaded, in-memory oracle grid (section 1 of the file format, §3.1) plus its sidecar metadata.
struct oracle_grid_entry {
  uint16_t nof_prb;
  uint8_t  nof_symbols;
  uint8_t  eaxc_id;
  std::vector<uint8_t> grid_payload;   // raw wire-format IQ, symbol-major (see §3.1 section 1)
  uint64_t tb_len_bytes;
  bool     tb_crc_ok;
  uint32_t rnti;
  uint32_t harq_id;
  uint32_t mcs_index;
  std::vector<uint8_t> tb_payload;     // ground truth, loader keeps it only for the harness-facing
                                        // dump path (§Error handling); ru_emulator's TX path never
                                        // reads tb_payload
};

// load_oracle_grid_set: parse+validate every file in `file_list` (magic, version, CRC32 trailer,
// iq_format consistency, grid_payload_len == nof_symbols*nof_prb*12*4 for the pinned uncompressed
// 16-bit format); returns error (not partial success) on any malformed file (P3-R2/R3, Error
// handling table).
oracle_grid_set load_oracle_grid_set(span<const std::string> file_list);

// select_symbol_payload: the per-burst patch point call, replacing generate_ul_uplane_messages()'s
// former reliance on the pre-generated random `test_data` payload region. `slots_per_frame` is
// derived from the numerology already known to ru_emulator (same source `slot_point` uses).
span<const uint8_t> select_symbol_payload(const oracle_grid_set& grids,
                                          unsigned sfn, unsigned slot, unsigned slots_per_frame,
                                          uint8_t eaxc_id, unsigned symbol_index);
// file_idx = (sfn * slots_per_frame + slot) mod grids.entries.size()   -- P3-R6, exact rule
// returns entries[file_idx].grid_payload sliced to [symbol_index]'s byte range
```

`select_symbol_payload`'s return value is copied into the frame's payload span at exactly the
offset/length `fill_random_data()` used to occupy today (`span<uint8_t>(frame).last(data_size)`,
`ru_emulator.cpp:286`) — same call site, same size contract, different source.

### M3 — gpu-phy `ingest_backend` (SIM implementation of the shared `oi_ingest` shape)

**Scope correction (2026-07-22, second pass — supersedes the first "M3 parses its own header"
draft):** `p2-phy-kernels` LLD §4.1 defines `oi_frame_desc` as **"32 bytes, filled by
ingest_backend, device-read"**. The parsing itself is **not** M3's own bespoke code — it calls the
shared `oi_oran_preparse_frame()` helper (owned by `p2a-scaffold`, `oi_oran_preparse.h/.cpp`),
designed to be usable by any ingest_backend implementation — **p6-physical-m1-ingest does not yet
call it** (a separate, still-open item; see the note in the code block below on the p3/p6
control-flow inversion). For this module, one implementation, one caller, so the bounded O-RAN CUS
header pre-parse logic (and its eventual real byte-offset fixes once tested against real wire
bytes — still open, Q2) lives in a place p6 *can* reuse later without duplicating it. M3's own job
is: own one
`oi_oran_preparse_state` per stream (MVP: one), call the helper per reaped frame, and call
`oi_p2_feed` with the descriptor the helper produced — nothing here re-derives `slot_id` or
re-parses header bytes independently of that shared call.

```
// Construction: bind SOCK_RAW/ETH_P_ALL on `iface`, attach a classic BPF program, enable
// PACKET_STATISTICS accounting (P3-R13).
//
// BPF FILTER SHAPE — updated 2026-07-24 (p2a/p2c's VLAN handling, triggered by p1's ru_emulator
// finding that --vlan_tag is CLI::Range(1,65536) with no untagged option, so the real wire is
// expected to always carry a tag): a single fixed-offset "EtherType==0xAEFE @ byte 12" filter
// silently drops every frame once a real 802.1Q tag is present (it shifts EtherType to byte 16).
// The filter MUST be the standard two-branch classic-BPF pattern, matching what
// oi_oran_preparse_frame (p2a-scaffold) already does per-frame on the host side:
//   (ethertype @ 12 == 0xAEFE) OR (ethertype @ 12 == 0x8100 AND ethertype @ 16 == 0xAEFE)
// i.e. accept BOTH untagged 0xAEFE-at-12 and 802.1Q-tagged 0x8100-at-12-with-0xAEFE-at-16 frames;
// do not gate on which one the live rig turns out to use (P1's Q1 remains open until a real
// capture confirms it, but the code must not assume either answer). This mirrors the exact
// two-path detection p2a-scaffold/src/host/oi_oran_preparse.cpp now implements (see that file and
// p2a/p2c VERIFICATION.md) — the BPF filter should pass through anything preparse can classify,
// and let preparse (not the kernel filter) make the final accept/reject call per-frame.
//
// IMPLEMENTATION CAVEAT (spec note now, code later): af_packet sometimes delivers a VLAN tag
// OUT-OF-BAND rather than inline in the frame bytes -- when the NIC/driver strips hardware VLAN
// offload, the kernel reports it via ancillary data (PACKET_AUXDATA / tp_status & TP_STATUS_VLAN_
// VALID, with the actual TCI in struct tpacket_auxdata::tp_vlan_tci) instead of leaving the 4-byte
// tag inline before the EtherType. If that happens, the frame bytes this module hands to
// oi_oran_preparse_frame would look untagged (14-byte header) even though a tag logically existed
// -- which is actually the SIMPLE case for preparse (it'll correctly compute eth_hdr_len=14), but
// only if the ingest wrapper is aware of the possibility and does one of: (a) do nothing, since an
// AUXDATA-delivered tag with an already-14-byte-effective frame needs no reinsertion for this
// pipeline's purposes (nothing downstream of K1 currently needs the VLAN ID itself); or (b) if a
// future consumer DOES need the VID, reinsert the 4 bytes from tp_vlan_tci before handing the
// frame to preparse, or equivalently pass an explicit eth_hdr_len=14 override and carry the VID
// out-of-band alongside the descriptor. Enable SOL_PACKET/PACKET_AUXDATA on the socket
// (setsockopt) and check TP_STATUS_VLAN_VALID on every recvmsg so this isn't silently missed --
// a live capture on the GCP VM (P1's next step) will show which delivery mode this rig's NIC/
// driver actually uses, but the ingest code should handle both without needing that answer first.
oi_ingest_handle oi_ingest_open_af_packet(const char* iface);

// Reaps as many frames as are queued (non-blocking). For each frame, in kernel-delivery order:
//  1. record RX timestamp (CLOCK_MONOTONIC_RAW) into this module's OWN internal log, keyed by
//     a monotonically increasing reap sequence number (oi_ingest_rx_log_entry, §Data structures
//     below) — NOT part of p2's ABI. p2's oi_p2_feed/oi_frame_desc carry no timestamp field by
//     design (kernels are wall-clock-agnostic); P3-R10's "RX timestamp" requirement is satisfied
//     by this local log, consumed only by P3-U3/P3-I1 verification tooling (M4/M5), never by p2.
//  2. copy the frame's raw bytes into p2's shared packet arena (I1, host-buffer allocation in
//     SIM per p2-phy-kernels HLD D10) at the next free arena_offset.
//  3. call oi_oran_preparse_frame(&this_stream_state, frame_bytes, frame_len, &desc) — the
//     SHARED helper (p2a-scaffold, owned there, not reimplemented here); it fills desc's
//     symbol_id/section_id/filter_index/start_prb/nof_prbs and derives slot_id from the
//     symbol-wrap (13->0) state it maintains internally. Malformed/truncated frames (non-OK
//     return) are dropped and counted (parse_failed, below) — same tolerance philosophy as K1's
//     own per-descriptor bounds check (p2-phy-kernels LLD §Error handling); this module does
//     NOT parse header bytes itself, only calls the shared helper.
//  4. set desc.arena_offset/desc.frame_len (the two fields the helper deliberately leaves for
//     the caller, since it doesn't know where ingest placed the bytes) and call
//     oi_p2_feed(pipeline, &desc) — the real p2 ABI (LLD §2: a fully-populated oi_frame_desc*,
//     not scalar args; feed() itself neither parses nor derives slot_id).
//     NOTE (still open, unrelated to today's fix): p6-physical-m1-ingest's oi_ingest_poll is
//     pull-based (compute_backend pulls descriptors FROM ingest) rather than push-to-feed() like
//     this module — a control-flow inversion between the two tiers' ingest_backend designs,
//     flagged in an earlier review pass and not yet reconciled. p6 SHOULD call the same shared
//     oi_oran_preparse_frame() helper to fill its own oi_frame_desc_t (both tiers producing the
//     same descriptor shape from the same parse logic is independently worth doing), but that
//     does not by itself resolve the push-vs-pull API-shape mismatch — do not treat this note as
//     having fixed that.
// Never transmits (P3-R8). Returns counters snapshot.
oi_ingest_counters oi_ingest_poll(oi_ingest_handle h);

struct oi_ingest_rx_log_entry {           // p3-internal only; never crosses the p2 ABI
  uint64_t reap_seq;                      // monotonic, matches oi_frame_desc write order
  uint64_t rx_ts_ns;                      // CLOCK_MONOTONIC_RAW at reap (P3-R10)
  uint32_t arena_offset;                  // cross-reference to the fed descriptor
};

struct oi_ingest_counters {
  uint64_t frames_seen;         // ETH_P_ALL total before ethertype filter
  uint64_t ethertype_matched;   // post-BPF-filter (0xAEFE)
  uint64_t parse_failed;        // ethertype-matched but common/section-header pre-parse rejected it
  uint64_t delivered;           // successfully handed to oi_p2_feed
  uint64_t feed_backpressure;   // oi_p2_feed returned backpressure (frame not delivered)
  uint64_t socket_drops;        // from PACKET_STATISTICS (tp_drops) — nonzero invalidates the run (P3-R13)
};

void oi_ingest_close(oi_ingest_handle h);
```

`oi_ingest_open_af_packet`/`oi_ingest_poll`/`oi_ingest_close` are p3's SIM-tier names for the
`oi_ingest` API shape; p6's PHYSICAL implementation (mlx5/dmabuf) will expose the same three-call
shape per SIM §3's backend-contract convention, with a different `_open_*` constructor.
`oi_p2_feed`/`oi_p2_drain`/`oi_frame_desc` themselves are **not** defined here — concrete names and
byte layout owned by `p2-phy-kernels` LLD §2/§4.1, restated (not redefined) for p3's own use.
`oi_oran_preparse_frame`/`oi_oran_preparse_state` are likewise **not** defined here — owned by
`p2a-scaffold` (`oi_oran_preparse.h`), called by this module, never reimplemented by it.

## Data structures & formats

### §3.1 Oracle RE-grid file format (`.osg`, "Oracle Slot Grid v1") — byte-precise

One file per entry in the configured, ordered `oracle_injection.files` list. All multi-byte
integer fields **little-endian** (host-authored file, not wire data). Section 1 (`grid_payload`)
is **wire-format** bytes and therefore follows O-RAN CUS-plane convention: signed 16-bit I/Q,
**network byte order (big-endian)**, packed symbol-major → PRB → subcarrier → (I, Q).

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `"OSG1"` (`0x4F 0x53 0x47 0x31`) |
| 4 | 2 | `format_version` (u16) | `1` |
| 6 | 1 | `iq_format` (u8 enum) | `0` = uncompressed 16-bit signed I/Q, no `udCompHdr` (pinned MVP value, P3-R3); other values reserved, loader rejects any non-zero value at MVP |
| 7 | 1 | `numerology_mu` (u8) | `1` (30 kHz SCS, pinned) |
| 8 | 2 | `nof_prb` (u16) | `51` (20 MHz, pinned) |
| 10 | 1 | `nof_symbols_per_slot` (u8) | `14` |
| 11 | 1 | `nof_eaxc` (u8) | `1` (MVP: single UL eAxC) |
| 12 | 2 | `eaxc_id` (u16) | `0` (pinned) |
| 14 | 2 | reserved | `0` |
| 16 | 8 | `tb_len_bytes` (u64) | length of `tb_payload` (section 2); `0` if this entry carries no ground-truth TB (e.g. a structural-only fill grid) |
| 24 | 4 | `tb_crc_ok` (u32, 0/1) | expected CRC24A verdict for this slot's TB |
| 28 | 4 | `rnti` (u32) | expected C-RNTI; MVP pins `0x4601` |
| 32 | 4 | `harq_id` (u32) | expected HARQ process id (new-data only, rv=0, per p2 MVP) |
| 36 | 4 | `mcs_index` (u32) | one of `{4, 13, 21}` (p2 MVP MCS set) |
| 40 | 8 | `grid_payload_len_bytes` (u64) | = `nof_symbols_per_slot * nof_prb * 12 * 4` for MVP (`14*51*12*4 = 34272`) |
| 48 | `grid_payload_len_bytes` | `grid_payload` | wire-format IQ, big-endian int16 pairs, order: for `symbol` in `0..nof_symbols_per_slot`, for `prb` in `0..nof_prb`, for `subcarrier` in `0..12`, emit `(I:int16_be, Q:int16_be)` |
| 48 + payload_len | `tb_len_bytes` | `tb_payload` | ground-truth TB bytes, TS 38.212 bit-to-byte packing (MSB-first); **read only by the harness (M4/M5), never by the ru_emulator loader (M2)** |
| end | 4 | `file_crc32` (u32, IEEE 802.3 poly) | over every preceding byte; loader and harness both verify before use |

M2's loader reads offsets 0–47 plus `grid_payload` only (`iq_format`/`nof_prb`/`nof_symbols_per_slot`/
`eaxc_id` cross-checked against the running config at load time, P3-R3's startup-error rule); it
never needs `tb_payload` and MAY skip reading it (seek past) to keep the RU-emulator process's
memory footprint independent of TB size. M4/M5 (the harness) read the full file, including
`tb_payload`, to compute the expected TB independently of the live decode (P3-R6/P3-R11 requirement
that the harness computes expected TB "independently").

**Reconciliation note (open, see §Open questions Q1):** `p2-phy-kernels` has no LLD.md yet (only
SPEC.md + HLD.md exist at the time this format was defined); its HLD.md documents K1's *internal*
pipeline RE-grid representation as `float2 grid[symbol][subcarrier]`, 14×612, fp32 (HLD "I2" row,
design decision D5). This `.osg` format's `grid_payload` is the **pre-K1, wire-level** 16-bit
fixed-point representation — the input K1 converts from, not the same object as K1's internal fp32
grid. The two are related by an exact, lossless conversion (16-bit fixed → fp32, per p2 HLD D5:
"Wire→fp32 conversion is exact"), so no numeric conflict exists, but the *file-level* format
defined here is p3's own definition, not copied from a p2 LLD that doesn't exist yet. When
`p2-phy-kernels/spec/LLD.md` is written, its author must confirm §3.1 here matches whatever
oracle-packed pcap generator format P2-R15b implies (both P2 and P3 need a "known RE grid → valid
U-plane bytes" packer; ideally the same packer/library, only invoked differently — P2 packs
directly into a pcap frame, P3's M2 packs into the same 48-byte-per-PRB layout ru_emulator itself
serializes at TX time).

### §3.2 Slot→file schedule (P3-R6)

```
file_idx(sfn, slot) = (sfn * slots_per_frame + slot) mod N
```
where `N = oracle_injection.files.size()` and `slots_per_frame` is derived from the pinned
numerology (µ=1 ⇒ 20 slots/frame, TS 38.211 Table 4.3.2-1). Deterministic and stateless: the
harness recomputes the same `file_idx` independently from a captured (sfn, slot) without any
shared runtime state with `ru-emu` (P3-R6's explicit purpose).

### §3.3 `oi_ingest_counters` (M3) — restated from Public APIs for the error-handling table below

See Public APIs §M3. `socket_drops` sources `PACKET_STATISTICS`'s `tp_drops` field
(`getsockopt(SOL_PACKET, PACKET_STATISTICS)`), reset each poll per standard af_packet semantics.

## Configuration (YAML)

### Extension to `ru_emu.yml` (additive-only; absence of the block = P3-R4 behavior)

**Corrected 2026-07-26 (real patch build + schema regression test, not the original draft
below):** this LLD's own first-draft example repeated the exact same two mistakes
`p1-ran-baseline/docker/configs/ru_emu.yml`'s header comment already found and corrected: (1) the
top-level shape is `ru_emu: cells: [...]`, not a flat `ru_emu: [...]` list (confirmed via
`ru_emulator_cli11_schema.cpp`'s real `--cells` mechanism); (2) the real CLI11/YAML option names
are `ru_mac_addr`/`du_mac_addr`/`compr_method_ul`/`compr_bitwidth_ul`/`ul_port_id`/`dl_port_id`/
`prach_port_id` (per `configure_cli11_ru_emu_args`), not `ru_mac_address`/`ul_compr_method`/
`ru_ul_port_id` etc. Caught by `p3-live-tap-ul-inject/tests/patch_schema_regression_test.cpp`
compiling the real patched schema and parsing this exact example -- fixed here, not silently
left wrong for the next reader.

```yaml
log: {level: info, filename: stdout}
ru_emu:
  cells:
    - network_interface: eth0
      ru_mac_addr: "02:6f:69:00:01:01"
      du_mac_addr: "02:6f:69:00:01:02"
      vlan_tag: 1
      bandwidth: 20
      compr_method_ul: none        # P3-R3 pin: was upstream default "bfp"; oracle injection requires
      compr_bitwidth_ul: 16        # uncompressed 16-bit, no udCompHdr — mismatch vs oracle files'
                                   # iq_format is a startup error (D4), not a silent re-encode
      ul_port_id: [0]
      dl_port_id: [0]
      prach_port_id: [4]
      # NO dpdk_config key — unchanged from P1 (socket transceiver, HLD D2)
      oracle_injection:            # NEW block (this feature). Entirely absent ⇒ upstream random-IQ
                                   # generator runs unmodified (P3-R4).
        enabled: true
        eaxc_id: 0                 # must be a member of ul_port_id
        files:                     # ordered list; index = schedule position (§3.2); N = len(files)
          - /oracle/slot_0000.osg
          - /oracle/slot_0001.osg
          - /oracle/slot_0002.osg
          # ... up to N entries; a run of ≥1000 injected slots (P3-R11 default) cycles this list
        fail_on_format_mismatch: true   # always true at MVP; documents the P3-R3 invariant, not a toggle
```

### gpu-phy tap config (new; feature-owned, not an upstream schema)

```yaml
gpu_phy:
  ingest:
    backend: af_packet          # SIM value; PHYSICAL (p6) uses a different backend key
    interface: eth0              # gpu-phy's fronthaul veth
    ethertype_filter: 0xAEFE
    # ^ the CONCEPTUAL filter value; the actual attached BPF program matches this ethertype at
    # EITHER byte 12 (untagged) OR byte 16 (802.1Q-tagged, TPID 0x8100 at byte 12) -- see the
    # oi_ingest_open_af_packet doc comment above for the full two-branch pattern and the
    # PACKET_AUXDATA caveat (2026-07-24). A single config key is enough because both branches
    # share the same target ethertype; only the offset varies per-frame, decided by the filter
    # itself, not by this config.
    # no promiscuous/interface-role knobs beyond BPF filter — SIM §3 ingest_backend is af_packet + memcpy
```

### Cross-consistency additions to `rigcfg_crosscheck.sh` (P1's script, extended, not replaced)

`oracle_injection.eaxc_id` ⊆ `ru_ul_port_id`; `ul_compr_method`/`ul_compr_bitwidth` match every
`.osg` file's `iq_format` (loader re-verifies this at runtime too — belt-and-suspenders, since
crosscheck runs pre-bring-up and files could change without a restart in a misconfigured rig).

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| Oracle file malformed (bad magic/version/CRC32) | M2 `load_oracle_grid_set` at startup | ru-emu process refuses to start; structured stderr error naming the file + which check failed (P3-R2 "no silent fallback" spirit, mirrors P1-R11-style hard failure) |
| Oracle file `iq_format` ≠ configured `ul_compr_method`/`bitwidth` | M2 at startup, cross-checked against `cfg.compr_params` | startup error, exit nonzero — **not** a silent re-encode (P3-R3, HLD D4) |
| Oracle file `nof_prb`/`nof_symbols_per_slot`/`eaxc_id` ≠ running cell config | M2 at startup | startup error, same class as above |
| Oracle file list exhausted mid-run | never — `file_idx` is a modulo over `N`; "exhaustion" is impossible by construction (it cycles). Explicitly noted: **if `N == 0` and injection is `enabled: true`**, that is a startup config error (empty list), not a runtime exhaustion event | startup error, exit nonzero, distinct message from the format errors above |
| `.osg` file's declared `grid_payload_len_bytes` doesn't match `nof_symbols_per_slot * nof_prb * 12 * 4` | M2 at load | startup error naming the file and both the declared and computed lengths |
| Tap `socket_drops` (`tp_drops`) nonzero during a run | M3 `oi_ingest_counters` snapshot at run end | run is invalid, per P3-R13 — the harness (M5) marks the whole session non-passing and requires a rerun; not a partial-credit result |
| Tap sees zero `0xAEFE` frames | M3 counters (`ethertype_matched == 0`) | harness fails fast with the same class of hint P1-R8's classifier uses (MAC-plan/hub-mode misconfiguration, D5) |
| O-RAN CUS header pre-parse rejects a frame (truncated, unexpected `filter_index`, section-header bounds violation) | M3 `oi_ingest_counters.parse_failed` increments; frame dropped, not fed | tolerated up to a point (matches K1's own "tolerates loss" philosophy, p2-phy-kernels LLD §Error handling) but any nonzero `parse_failed` during a P3-I1 run invalidates bit-exact accounting for the same reason as `socket_drops`/`feed_backpressure` — a dropped, unaccounted frame is indistinguishable from a real decode bug |
| `oi_p2_feed` backpressure returned | M3 `feed_backpressure` counter increments; frame is dropped from delivery (by contract, non-blocking) | any nonzero `feed_backpressure` during a P3-I1 run invalidates the bit-exact accounting for the same reason as `socket_drops` (a decoded-vs-injected count mismatch would otherwise be indistinguishable from a real bug) — treated identically to P3-R13's socket-drop rule even though it is not literally a socket drop |
| C-plane pcap structural diff (patched vs upstream, P3-R5 verification) found | M4-adjacent diff tool, run once per patch revision, not per session | patch review blocks merge; not a runtime failure mode |
| Patch fails `git apply --check` against a fresh pinned checkout (P3-R1) | CI | CI red; patch series must be rebased |

## Test plan (per requirement)

| Req | Test |
|---|---|
| **P3-R1** | CI: `git apply --check` of the full patch series against a freshly cloned, tag-pinned `release_26_04` checkout; green required pre-merge. |
| **P3-R2** | Unit (M2): construct a synthetic `.osg` set, enable injection, assert every UL section byte-for-byte equals the corresponding `.osg` `grid_payload` slice, for all configured symbols/eAxC — no random-generator bytes appear anywhere in the TX path. |
| **P3-R3** | Unit: (a) matched config → 0 mismatches on a captured burst (byte comparator, M4); (b) deliberately mismatched `ul_compr_bitwidth` vs a `.osg` file's `iq_format` → process exits with the documented startup error, **not** a re-encoded/silently-adjusted payload (assert on both exit code and absence of any successfully-sent frame). |
| **P3-R4** | Regression: injection `enabled: false` (or block absent) → same config schema accepted (schema unit test) + rerun of P1's full P1-G1/P1-G2 gate suite against the patched binary, byte-for-byte same pass/fail outcomes as the upstream binary. |
| **P3-R5** | (a) Code review diff-scope check (patch touches only the identified TX-payload path, M2, and config/YAML parsing — CI greps the diff for any touched line inside C-plane decode/generation or DL-handling functions and fails if found); (b) captured C-plane pcap, patched vs upstream `ru-emu` binary, same session shape: field-by-field structural compare shows zero differences. |
| **P3-R6** | Unit (M2/M5 shared logic): for a range of (sfn, slot) pairs spanning >N, assert `file_idx` matches the documented modulo formula (§3.2) in both the patch's internal computation and the harness's independent computation, and that they agree with each other without any shared runtime state. |
| **P3-R7** | Unit (M3), replayed into a veth pair: canned frame mix (0xAEFE + other ethertypes) → `oi_ingest_poll` delivers only the 0xAEFE-matched frames via `oi_p2_feed`, counters (`frames_seen`/`ethertype_matched`/`delivered`) reconcile exactly. |
| **P3-R8** | Static: code/lint check that M3 never calls a transmit path (no `send`/`write` on the tap socket beyond the bind/filter setup calls); dynamic: tcpdump on `gpu-phy`'s own veth during a full session shows zero frames originated by `gpu-phy`. |
| **P3-R9** | Integration: full rig up with hub-mode (or `tc mirred` fallback) active; count U-plane TX frames at `ru-emu` (`tx_total_counter`, existing upstream KPI) vs frames delivered at the tap (`oi_ingest_counters.ethertype_matched`) over a counted run — equal. |
| **P3-R10** | Unit (M3): frames delivered to `oi_p2_feed` preserve kernel `recvmmsg`/reap order (sequence-number check using the O-RAN `seq_id` field already in each frame) and each carries a monotonically non-decreasing `CLOCK_MONOTONIC_RAW` timestamp. |
| **P3-R11** | Integration (M5), the core bit-exact gate: run ≥1000 injected UL slots; for every drained `oi_p2_tb_record` (real fields: `slot_id, tb_size_bytes, nof_cb, base_graph, crc24a_ok, mcs_index, tb_data` — no `sfn`/`rnti`/`harq_id` in the record itself, per SPEC.md §Dependencies), recompute `file_idx` from `slot_id` (via the pinned `slots_per_frame`), load that `.osg`'s `tb_payload`/`tb_crc_ok` and the pinned-config `rnti`/`harq_id` constants (not per-record), and assert byte-identical TB (`tb_data` vs `tb_payload`) + matching CRC verdict (`crc24a_ok` vs `tb_crc_ok`). **Zero mismatches** required (not a statistical threshold) — this is the direct DEV-044 fix: DEV-044 established that srsRAN/OCUDU emulator and benchmark defaults are synthetic/ground-truthless by default (`ldpc_decoder_benchmark`'s random-LLR mode, `ru_emulator`'s static-IQ mode, same shape per `SRSRAN_LIMITATIONS.md` §4–§5), so any comparison weaker than byte-identical-vs-independently-known-truth would silently reintroduce exactly the "looks real, isn't" gap this whole feature exists to close. A tolerance-gated or CRC-only check would pass even if the pipeline decoded noise that merely happened to look plausible; only bit-exact-vs-oracle rules that out. |
| **P3-R12** | Integration: reuse P1's `soak_stability.sh`/log-diff baseline unmodified, run for 10 minutes concurrently with the P3-R11 session; zero new ERROR-level `gnb` lines vs the P1 baseline pattern list, zero container restarts, stable KPI counters — identical pass criteria to P1-G2. |
| **P3-R13** | Integration: `oi_ingest_counters.socket_drops` sampled at run end; any nonzero value marks the run **invalid** (harness refuses to report a P3-R11 verdict and requires a rerun) — asserted by a dedicated harness unit test that injects a synthetic nonzero-drop counter and confirms the harness refuses to emit pass/fail. |
| **P3-R14** | Static lint (extends P1's `lint_no_perf.sh` pattern list over this feature's tree): any timing figure printed by M4/M5 tooling must carry the literal string `SIM — not quotable`; grep-able CI check. |

**Gate mapping** (SPEC.md Acceptance gates): **P3-U1** = {R3, R6} · **P3-U2** = {R1, R4, R5} ·
**P3-U3** = {R7, R10, R13} · **P3-I1** = {R9, R11, R12, R13} run together in one live session.

## Open questions

1. **Q1 — `.osg` format vs `p2-phy-kernels` LLD (reconciliation, see §3.1 note).** `p2-phy-kernels`
   has no LLD.md yet. When it lands, confirm whether P2-R15b's "oracle-packed" pcap generator and
   this feature's M2 loader should share one packer library (grid → wire bytes) with two front
   ends (pcap-frame writer for p2, `.osg`-file writer/reader for p3), or remain independent
   implementations of the same byte layout. Recommend the shared-library approach to avoid two
   places encoding "51 PRB × 14 symbols × 12 subcarriers × (16-bit I, 16-bit Q), big-endian" that
   could silently drift apart.
2. **Q2 — Oracle TB/grid generation tool itself is out of this feature's scope.** This LLD defines
   the *file format* the loader and harness consume, but not the offline encoder that produces
   `.osg` files from a chosen TB (LDPC encode → rate-match → scramble → QAM map → RE placement,
   the inverse of p2's decode chain). That tool is either a p2-adjacent deliverable (it is
   effectively p2's encode-side twin) or a p3-owned harness utility; owner to be assigned before
   implementation begins — flagged here rather than assumed.
3. **Q3 — Multi-eAxC oracle sets.** MVP pins `nof_eaxc = 1` (matching p2's MVP). The `.osg` format
   reserves `nof_eaxc`/`eaxc_id` fields for a future multi-eAxC extension (one file per eAxC per
   schedule slot, or one file covering all eAxCs) but the exact multi-file vs multi-section
   grouping is undecided and unnecessary until p2 supports >1 rx port.
4. **Q4 — Hub-mode vs `tc mirred` decision timing (inherits P1 LLD Q2).** Whether Docker's bridge
   driver on WSL2/GCP honors `ageing_time 0` cleanly enough for zero-loss flooding to `gpu-phy`, or
   whether the `tc mirred` fallback (HLD D5) is needed on one or both hosts, is an implementation-
   time finding, not a design choice this LLD can pre-resolve. Recorded here so P3-R9's test
   doesn't get treated as a surprise if the primary mechanism needs the fallback.
5. **Q5 — `slots_per_frame` source of truth.** §3.2's schedule formula needs `slots_per_frame`
   from the pinned numerology (µ=1 ⇒ 20). Whether M2 derives this from OCUDU's own `slot_point`/
   numerology helpers (consistent with how `ru_emulator.cpp` already computes symbol duration,
   `ru_emulator.cpp:350-351`) or hardcodes it as a config-validated constant is an implementation
   detail; either is consistent with this LLD as long as M5's independent computation uses the
   identical value (cross-checked at harness startup against the rig config, not assumed).
