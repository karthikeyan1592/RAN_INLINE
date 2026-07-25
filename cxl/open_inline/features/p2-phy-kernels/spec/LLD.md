# p2-phy-kernels — LLD

Companion to [`SPEC.md`](SPEC.md) / [`HLD.md`](HLD.md). Design document; paths are the layout the
implementation must produce. Kernel argument lists and structs below are grounded directly in
`third_party/ocudu` (BSD-3, `release_26_04`, HEAD `4e9f8d6`) — see the per-kernel "Port grounding"
line. Interface numbers (I1–I10) and component names (`oi_p2_host`, `oi_kernel_compat.h`) are
carried unchanged from HLD §3/§2; this file does not rename anything HLD already committed to.

## 1. Module breakdown

```
features/p2-phy-kernels/
  src/
    kernels/
      k1_depacketizer.cl        # T4 fresh — O-RAN U-plane parse + RE-grid scatter (P2-R3)
      k2_chanest.cl             # T3 port — DMRS LS + interp (P2-R4)
      k3_equalizer.cl           # T3 port — MMSE 1x1 (P2-R5)
      k4_demapper.cl            # T3 port — soft demap -> int8 LLR (P2-R6)
      k5_descrambler.cl         # T3 port — Gold sequence + sign-flip (P2-R7)
      k6_rate_dematcher.cl      # T3 port — bit-selection revert + deinterleave (P2-R8)
      oi_kernel_compat.h        # single abstraction header (HLD §2); MVP: empty of vendor branches
    host/
      oi_p2_host.h/.cpp         # setup/feed/drain API (P2-R17), queue/event orchestration (HLD §6)
      oi_p2_config.h/.cpp       # YAML load + MVP-config validator (P2-R11), rejects on parse
      oi_p2_buffers.h/.cpp      # buffer pool: RE grid x2, chan-est, eq-out, LLR, CB-LLR (HLD §5)
      oi_p2_cb_segment.h/.cpp   # host CB (de)segmentation params (TS 38.212 §5.2.2) (P2-R10)
      oi_p2_crc.h/.cpp          # CRC24A/24B wrapper, ported algorithm from crc_calculator_generic_impl
      oi_p2_provenance.h        # emits provenance.json per kernel (P2-R12)
      oi_frame_desc.h           # I1 descriptor type (§4.1); plain-C type, crosses the C/C++ boundary
      oi_oran_preparse.h/.cpp   # shared O-RAN header pre-parse (§2/§4.1) — called by ingest_backend
                                #   (p3/p6) only, never by oi_p2_feed itself; owns the symbol-wrap
                                #   slot_id derivation state machine
    ldpc/                       # NOT built here — links the existing bit-exact BG1/BG2 kernel (dependency, P2-R9)
  docker/
    (none new — runs inside p0's `gpu-phy` image; oracle harness in p0's `oracle` image)
  helpers/
    oracle_compare.py           # dual-oracle comparator CLI (srsRAN CI-only + Sionna) (P2-R14)
    pcap_packer.py               # packs oracle RE grids into valid U-plane frames (P2-R15b)
    lint_portability.py          # static lint for P2-R2 (warp-width/asm/intrinsic grep + WG-size scan)
    provenance_check.py          # CI gate: every kernel source has a provenance.json entry (P2-R12)
    agpl_denylist.py             # packaging/lint deny-list check (P2-R13)
  tests/
    unit/k{1..6}_test.py         # per-kernel dual-oracle harness (P2.1–P2.6 gates)
    integration/pipeline_test.py # growing-pipeline pcap decode (P2-R15)
    oclgrind/                    # nightly memory-safety job wrapper
```

Each kernel file + its unit test is buildable/testable standalone at every pipeline prefix
(P2-R1): `k1_depacketizer.cl` alone produces I2 and is gated by P2.1 before `k2_chanest.cl` exists.

## 2. Public APIs (host orchestration — `oi_p2_host`, stable per P2-R17)

Signatures are C-linkage (OpenCL-host-callable from C or C++; SYCL variant wraps the same shape —
HLD §6 "two thin enqueue adapters"). All buffers referenced by handle, not raw pointer, so the
SIM/PHYSICAL `ingest_backend`/`handoff_backend` swap (I1, I7) is invisible here.

```c
// oi_p2_host.h

typedef struct oi_p2_pipeline oi_p2_pipeline;   // opaque

typedef enum {
  OI_P2_OK = 0,
  OI_P2_ERR_CONFIG_REJECTED   = 1,  // P2-R11: config != MVP
  OI_P2_ERR_CL_BUILD_FAILED   = 2,
  OI_P2_ERR_ARENA_OVERFLOW    = 3,  // I1 descriptor ring full
  OI_P2_ERR_SLOT_INCOMPLETE   = 4,  // K1 bitmap not all-symbols-seen at drain deadline
  OI_P2_ERR_ORACLE_MISMATCH   = 5,  // test/CI builds only
  OI_P2_ERR_DEVICE            = 6,  // clGetDeviceInfo / event error propagated from ICD
} oi_p2_status;

/// Parses+validates a YAML config (Section 4) against the fixed MVP shape and, on success,
/// builds all six kernel programs + allocates all pipeline-lifetime buffers (HLD §5: zero
/// device allocation after this call). Returns a ready, empty pipeline.
oi_p2_status oi_p2_setup(const char* yaml_path, cl_context ctx, cl_command_queue q,
                         oi_p2_pipeline** out_pipeline);

/// Appends one already-parsed frame descriptor to the packet arena ring (I1). `desc` must be
/// **fully populated by the caller** — `oi_p2_feed` does not parse header bytes and does not
/// derive `slot_id`; the frame's raw bytes must already be in the arena at `desc->arena_offset`
/// (placed there by ingest_backend: pcap replay in SIM, NIC writes in PHYSICAL). The caller
/// (ingest_backend) derives every `oi_frame_desc` field via the shared `oi_oran_preparse()`
/// helper (p2a-scaffold) before calling this. **Reconciled 2026-07-22** (superseding an earlier
/// draft that took `(slot_id, arena_offset, len)` scalars): that signature was circular — `slot_id`
/// itself is derived from watching `symbol_id` wrap 13→0, which requires having already parsed
/// the header, so a version of `feed()` that parsed internally would need `slot_id` before it
/// could legitimately have one. Moving parsing into `feed()` also cannot work for PHYSICAL's
/// dmabuf ingest, where frame bytes land directly in GPU-visible memory the host cannot cheaply
/// read arena-side. `desc` is copied; the caller may reuse/free it immediately after return.
/// Non-blocking; returns after the descriptor is appended to the ring.
oi_p2_status oi_p2_feed(oi_p2_pipeline* p, const oi_frame_desc* desc);

/// Signals slot `slot_id` is complete (all expected frames fed, or timeout) and enqueues the
/// K1..K6+LDPC chain for that slot on the pipeline's in-order queue (HLD §6). Non-blocking;
/// the resulting cl_event is stored internally and surfaced via oi_p2_drain.
oi_p2_status oi_p2_launch_slot(oi_p2_pipeline* p, uint32_t slot_id);

/// Blocks until slot `slot_id`'s host CRC tail (CB desegment, CRC24B/24A) has run and writes
/// the TB+CRC record (Section 3.6) into `out_record`. This is the I8/I9 boundary p4-phy-l2-seam
/// consumes. Returns OI_P2_ERR_SLOT_INCOMPLETE if the slot was never launched.
oi_p2_status oi_p2_drain(oi_p2_pipeline* p, uint32_t slot_id, oi_p2_tb_record* out_record);

/// Optional debug tap (I10): reads back any of I2-I6 for the given slot into a caller buffer.
/// Used only by the oracle harness; never on the pass/fail critical path of P2-R1..R11,R15..R17.
oi_p2_status oi_p2_tap(oi_p2_pipeline* p, uint32_t slot_id, int stage_id /*I2..I6*/,
                       void* out_buf, size_t out_buf_bytes);

void oi_p2_teardown(oi_p2_pipeline* p);
```

`oi_p2_launch_slot` is the point where D4 (slot-granular batching) and §6's "one in-order
command queue... explicit `cl_event` per launch" are realized: it enqueues K1→K2→K3→K4→K5→K6→
LDPC→readback as one dependency chain and returns without blocking; `oi_p2_drain` is the only
blocking call, matching HLD §6 exactly ("host blocks only on the readback event").

## 3. Device kernel prototypes (OpenCL C; SYCL generic-SSCP mirrors 1:1 per kernel rule, SIM §3)

All kernels take `__global` buffer pointers sized per Section 4's MVP dimensions (K1 in "In
scope"/HLD D5 §8: fp32 RE grid internally, never OCUDU's `cbf16_t`). Work-item/work-group shapes
below are the MVP mapping (HLD §4 steps 2–7); no kernel reads `get_sub_group_size()` or assumes a
particular one (P2-R2).

### K1 — depacketizer + RE-grid reassembly (T4 fresh; port grounding: none — O-RAN CUS U-plane
spec fields as named in OCUDU's `uplane_message_params`/`uplane_section_params`,
`include/ocudu/ofh/serdes/ofh_uplane_message_decoder_properties.h`, consulted read-only for field
semantics per HLD D9)

```c
// work-item = one U-plane section's RE span slice (HLD §4 step 2)
__kernel void k1_depacketize(
    __global const uchar*        arena,           // I1: raw Ethernet frames, byte buffer
    __global const oi_frame_desc* descs,          // I1: descriptor ring (Section 4.1), nof_descs entries
    uint                          nof_descs,
    uint                          slot_id,         // for descriptor slot-membership filtering
    __global float2*              re_grid,         // I2 out: [symbol][subcarrier], 14*612 float2
    __global uint*                symbol_bitmap);  // I2 out: 14-bit completeness bitmap, atomic-OR'd
```

### K2 — DMRS LS chan-est + interpolation (T3 port; port grounding:
`lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.{h,cpp}` `estimate()`;
`lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.*`;
`lib/phy/support/interpolator/interpolator_linear_impl.*`)

```c
// work-group = one DMRS symbol (HLD §4 step 3); ported algorithm from
// dmrs_pusch_estimator_impl::estimate/sequence_generation + port_channel_estimator_average_impl
__kernel void k2_chanest(
    __global const float2* re_grid,        // I2 in
    __global const uint*   symbol_bitmap,  // I2 in
    __global const float2* dmrs_ref_seq,   // generated ref sequence (TS 38.211 §6.4.1.1.1), per DMRS symbol, 612 REs
    uint                    dmrs_symbol_idx,// one of {2,7,11} for this launch
    __global float2*       ch_est,         // I3 out: per-data-RE estimate, data-RE-linear order
    __global float*        noise_var,      // I3 out: per-slot scalar (pilot-residual derived)
    __global float*        epre);          // I3 out: per-slot scalar
```

### K3 — MMSE equalizer, 1x1 (T3 port; port grounding:
`lib/phy/upper/equalization/channel_equalizer_generic_impl.{h,cpp}` `equalize()`, MMSE branch;
scalar-loop structure ported from `equalize_zf_1xn.h`'s per-RE accumulation pattern, generalized
to the MMSE `1/(|h|^2+sigma^2)` denominator)

```c
// work-item = one data RE (HLD §4 step 4)
__kernel void k3_equalize(
    __global const float2* re_grid,    // I2 in (data REs only, DMRS symbols skipped by index map)
    __global const float2* ch_est,     // I3 in
    __global const float*  noise_var,  // I3 in, per-slot scalar (RX_PORTS=1 => no port loop)
    float                   tx_scaling,
    __global float2*       eq_symbols, // I4 out: data-RE-linear order
    __global float*        eq_noise_var); // I4 out: per-RE post-eq noise variance
```

### K4 — soft demapper -> int8 LLR (T3 port; port grounding:
`lib/phy/upper/channel_modulation/demodulation_mapper_impl.{h,cpp}` `demodulate_soft()` dispatch +
`demodulation_mapper_{qpsk,qam16,qam64}.cpp` scalar paths; `log_likelihood_ratio::quantize()`,
`include/ocudu/phy/upper/log_likelihood_ratio.h`)

```c
// work-item = one data RE; emits Qm LLR bits per RE (HLD §4 step 5)
__kernel void k4_demap(
    __global const float2* eq_symbols,    // I4 in
    __global const float*  eq_noise_var,  // I4 in
    uint                    qm,            // 2 (QPSK) | 4 (16QAM) | 6 (64QAM); MVP MCS-derived
    __global char*          llr_out);      // I5 out: int8, RANGE_LIMIT_FLOAT = 24(qm=2)/20(qm=4,6),
                                            //          LLR_MAX=120, saturate to +-120, never +-127 here
```

### K5 — descrambler (T3 port; port grounding:
`lib/phy/upper/sequence_generators/pseudo_random_generator_impl.{h,cpp}` `apply_xor(span<log_likelihood_ratio>,...)`,
`init(unsigned c_init)`, fast-advance from `pseudo_random_generator_fast_advance.h`)

```c
// work-item = one LLR block (HLD §4 step 6); c_init computed host-side once per codeword (D4:
// slot batching collapses OCUDU's per-symbol incremental scrambling state to one c_init/codeword)
__kernel void k5_descramble(
    __global char* llr_inout,   // I5 in-place: int8 LLR, sign-flipped by Gold sequence bit
    uint            c_init,      // = RNTI * 2^15 + n_ID (TS 38.211 §6.3.1.1)
    uint            nof_llrs);
```

### K6 — rate-dematcher, single-shot (T3 port; port grounding:
`lib/phy/upper/channel_coding/ldpc/ldpc_rate_dematcher_impl.{h,cpp}` `rate_dematch()` — generic
class only, never `ldpc_rate_dematcher_{avx2,avx512,neon}_impl` per D3)

```c
// work-item = one output CB position (HLD §4 step 7); new_data=true always (P2 scope: rv=0, no HARQ)
__kernel void k6_rate_dematch(
    __global const char*            llr_in,      // I5 in: rate-matched LLRs for this CB
    uint                              rm_length,   // codeblock_metadata::cb_specific.rm_length
    uint                              full_length,  // codeblock_metadata::cb_specific.full_length (66*Zc BG1 / 50*Zc BG2)
    uint                              nof_filler_bits,
    uint                              shift_k0,     // TS 38.212 Table 5.4.2.1-2, rv=0 always in MVP
    uint                              modulation_order, // 1/2/4/6 (BPSK/QPSK/16QAM/64QAM code path select)
    __global char*                    cb_llr_out);  // I6 out: full-size CB LLR buffer, filler = +LLR_INFTY (127)
```

LDPC decode (reused, unmodified source) and CPU tail (CB desegment + CRC24B/24A, host code
modeled on `pusch_codeblock_decoder::decode()`'s dematch→decode→CRC sequencing but with the
dematch step already done by K6) are dependencies, not new device kernels — no prototype here
(P2-R9, P2-R10).

## 4. Data structures & formats

### 4.1 I1 — packet arena descriptor (`oi_frame_desc`, 32 bytes, **filled by ingest_backend**, device-read)

**Field ownership (reconciled 2026-07-22):** every field is derived by the ingest_backend
implementation (p3 in SIM, p6 in PHYSICAL) via the shared `oi_oran_preparse()` helper
(p2a-scaffold, `oi_oran_preparse.h`) and passed fully populated to `oi_p2_feed` (§2). Neither
`oi_p2_feed` nor any pipeline kernel parses header bytes or derives `slot_id` — see §2's `feed`
doc comment for why (the old "host-filled" wording didn't say *which* host-side code fills it,
and the natural reading — inside `feed()` — turns out to be circular and PHYSICAL-incompatible;
p2a-scaffold's `VERIFICATION.md` records the finding).

| Offset | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 8 | `arena_offset` | byte offset into arena buffer |
| 8 | 4 | `frame_len` | raw Ethernet frame length, bytes |
| 12 | 4 | `slot_id` | host slot counter (wraps at 2^32; not the 3GPP `slot_point` bit-packing); derived by `oi_oran_preparse()` from `symbol_id` wrap detection (13→0), never by `feed()` |
| 16 | 1 | `symbol_id` | 0–13, from O-RAN U-plane section header (`uplane_message_params::symbol_id`) |
| 17 | 1 | `section_id` | from U-plane section header |
| 18 | 1 | `filter_index` | from common header; MVP expects one fixed value, else reject (P2-R11 scope) |
| 19 | 1 | `flags` | bit0 `is_every_rb_used`, bit1 `use_current_symbol_number`, bits2-7 reserved=0 |
| 20 | 2 | `start_prb` | 0–50 (MVP: always 0, full-band) |
| 22 | 2 | `nof_prbs` | 1–51 (MVP: always 51) |
| 24 | 8 | reserved | zero-filled; future eAxC/VLAN/BFP fields land here without ABI break |

Fixed-point IQ payload (MVP: uncompressed 16-bit, D6): each RE = `int16 I, int16 Q` big-endian on
the wire (O-RAN CUS convention); K1 converts wire → `float2` via `float(be16_to_native(x)) /
32767.0f` — an exact, lossless widening (D5: "wire→fp32 conversion is exact ⇒ K1 stays
integer-exact-testable" after this conversion, per P2-R3 gate wording "integer-exact after
fixed-point→float conversion"). **Corrected 2026-07-22 (p2c-k1):** this section previously said
`/ 32768.0f` (a clean Q15/power-of-2 assumption, never checked against a real encoder). OCUDU's
real quantizer (`lib/ofh/compression/quantizer.h`) uses `gain = (1 << (bit_width-1)) - 1.0f =
32767.0f` for 16-bit — confirmed empirically in `p2c-k1/tests/k1_test.cpp` by round-tripping
through the real OCUDU builder + decoder. `32767.0f` also matches the gain K4's own
`RANGE_LIMIT_FLOAT` constants (§4.5) are implicitly calibrated against, since those come from
real OCUDU demodulation code operating on the same convention.

### 4.2 I2 — RE grid (slot)

`float2 re_grid[14][612]` (14 symbols x 612 subcarriers, µ=1/20MHz per SPEC MVP table), row-major
by symbol then subcarrier; `uint32 symbol_bitmap` — bit *s* set iff symbol *s* fully populated.
DMRS symbols {2,7,11} carry DMRS REs on odd/even subcarrier half per CDM group (type 1, 2 CDM
groups w/o data, TS 38.211 Table 6.4.1.1.3-1); the other 11 symbols carry PUSCH data at all 612
subcarriers (full-band allocation).

### 4.3 I3 — channel estimate

`float2 ch_est[N_data_re]` where `N_data_re = 11 * 612 = 6732` (data-RE-linear order: symbol-major
over the 11 non-DMRS symbols, subcarrier-minor); `float noise_var` (per-slot scalar, MVP: 1 rx
port so no per-port array); `float epre` (per-slot scalar). Time-domain hold: symbols in
{0,1}→est(sym 2), {3..6}→hold between est(2)/est(7) or nearest, {8..10}→hold, {12,13}→est(11)
(HLD §4 step 3 "time-domain hold between DMRS symbols").

### 4.4 I4 — equalizer output

`float2 eq_symbols[N_data_re]`, `float eq_noise_var[N_data_re]` — same data-RE-linear order as I3.

### 4.5 I5 — LLR stream (K4→K5→K6)

`int8 llr[N_data_re * Qm]`, OCUDU's `log_likelihood_ratio` value_type (`include/ocudu/phy/upper/log_likelihood_ratio.h:30`):
finite range `[-120, +120]` (`LLR_MAX = 120`), reserved values `±127` (`LLR_INFTY`) denote a
certainly-0/certainly-1 bit (filler bits from K6, never emitted by K4). Quantization per RE
(`log_likelihood_ratio::quantize(value, range_limit)`, mid-tread uniform, step =
`range_limit/LLR_MAX`, values beyond `±range_limit` saturate to `±LLR_MAX` — never to
`±LLR_INFTY`): `range_limit = 24.0f` for QPSK (Qm=2), `20.0f` for 16QAM/64QAM (Qm=4/6) — exact
constants from `demodulation_mapper_{qpsk,qam16,qam64}.cpp` `RANGE_LIMIT_FLOAT`. Codeword bit
order: per-RE, Qm bits MSB-first as OCUDU emits them (matches `demodulate_soft`'s per-symbol bit
ordering — no reordering introduced by K4/K5).

### 4.6 I6 — CB LLR buffers (K6→LDPC)

`int8 cb_llr[N]` per CB, `N = 66*Z_c` (BG1) or `50*Z_c` (BG2), `Z_c` = lifting size selected by TB
size/BG per TS 38.212 §5.2.2 (host-computed, `codeblock_metadata::tb_common.lifting_size`).
Filler-bit positions (`cb_specific.nof_filler_bits`, always at the systematic-bit tail per TS
38.212 §5.2.2) are set to `+LLR_INFTY` (127) — P2-R8. This is the existing LDPC kernel's expected
input ABI unchanged (P2-R9: "same LLR ABI, no re-quantization between K6 and LDPC").

### 4.7 I8 — TB+CRC record (`oi_p2_tb_record`, host tail output, consumed by p4-phy-l2-seam)

| Offset | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 4 | `schema` | `0x0002_0001` = "oi-p2-tb/1" |
| 4 | 4 | `slot_id` | matches `oi_p2_drain` argument |
| 8 | 4 | `tb_size_bytes` | TS 38.212 §5.1 TB size (MVP-config + MCS derived) |
| 12 | 1 | `nof_cb` | 1 if TB fits without segmentation (CRC16 only path), else per §5.2.2 |
| 13 | 1 | `base_graph` | 1 or 2 |
| 14 | 1 | `crc24a_ok` | bool: 1 = TB CRC24A pass |
| 15 | 1 | `mcs_index` | 4, 13, or 21 (MVP set) |
| 16 | `tb_size_bytes` | `tb_data` | packed transport block bits, byte-aligned MSB-first |
| variable | 3 | `crc24a` | the 24-bit CRC24A value itself, for oracle taps / debugging (not re-verified downstream) |

Per-CB CRC24B verdicts (only meaningful when `nof_cb > 1`) are host-internal to the desegmentation
step and not carried in this record — only the aggregate `crc24a_ok` crosses I8 (matches P2-R10's
"CRC24B per CB (>1 CB) and CRC24A over the TB" wording: 24B gates desegmentation, 24A is the
externally-visible verdict).

## 5. Configuration (YAML/env schema)

Single YAML, loaded by `oi_p2_setup`; any key outside this exact shape, or any value outside the
listed set, is a hard rejection (`OI_P2_ERR_CONFIG_REJECTED`, P2-R11) — no partial/best-effort
support.

```yaml
schema: oi-p2-config/1
duplex: tdd
band: n78
numerology: 1            # mu=1, 30 kHz SCS -- only accepted value
bandwidth_mhz: 20         # -> 51 PRB / 612 subcarriers, only accepted value
nof_ues: 1
nof_layers: 1
nof_rx_ports: 1
pusch_allocation:
  rb_start: 0
  nof_prb: 51
  symbol_start: 0
  nof_symbols: 14
  mapping_type: A
dmrs:
  type: 1
  additional_positions: 2   # -> symbols {2,7,11}, l0=2
  cdm_groups_without_data: 2
mcs_set: [4, 13, 21]        # TS 38.214 Table 5.1.3.1-1 indices; Qm = 2/4/6 respectively
harq:
  new_data_always: true
  rv: 0
uci_on_pusch: none
scrambling:
  n_id: 1                   # = PCI
  rnti: 0x4601
ofh:
  eaxc: [0]
  compression: uncompressed_16bit
  vlan: false
oracle:
  srsran_vectors_path: /third_party/srsRAN_Project   # CI-only bind mount; absent in shippable image
  sionna_vectors_path: /vectors/sionna                # shippable
device:
  platform_env: OI_CL_PLATFORM   # e.g. "pocl"; resolved at oi_p2_setup, not hardcoded
```

Validator (`oi_p2_config.cpp`) checks, in order: schema tag match; every scalar field equals its
single accepted MVP value (table in SPEC "Fixed MVP configuration"); `mcs_set` is exactly `{4, 13,
21}` in any order; `oracle.sionna_vectors_path` must exist (required for shippable gate),
`oracle.srsran_vectors_path` is optional (its absence downgrades P2-R14a to "skipped, logged" —
never a hard failure, since that oracle is CI-only per P2-R13). First violation short-circuits
with a structured error naming the offending key and both the read and expected value.

## 6. Error handling

| Failure class | Detection point | Behavior |
|---|---|---|
| Config outside MVP shape | `oi_p2_setup` YAML validation | `OI_P2_ERR_CONFIG_REJECTED`, structured message (key, got, expected); no partial pipeline built (P2-R11) |
| OpenCL C build/JIT failure (any kernel) | `oi_p2_setup`, `clBuildProgram` return + build log | `OI_P2_ERR_CL_BUILD_FAILED`; full ICD build log surfaced to caller; setup aborts before any buffer allocation |
| Oclgrind finding (invalid read/write, race) | nightly Oclgrind CI job, not runtime | job fails; report attached with kernel name + offending access; does not block PoCL-only per-commit gates (P2-R2 escalation path) |
| Malformed/truncated U-plane frame | K1, per-descriptor bounds check against `arena_offset+frame_len` vs section header's declared `nof_prbs`/`ud_comp_len` | frame dropped, not fatal; symbol bitmap bit for that (symbol, section) left unset; slot proceeds with partial completeness (matches P2-R3 "tolerates loss/reorder/duplication") |
| Duplicate frame (same slot/symbol/section twice) | K1, idempotent scatter keyed by (slot,symbol,section) | second write wins (last-write, not summed); bitmap bit already set, no double-count |
| Reordered frames (symbol N+1 arrives before N) | K1 scatter is symbol-indexed, not stream-ordered | no special handling needed — scatter target is computed from the descriptor's own `symbol_id`, order-independent by construction |
| Packet arena / descriptor ring full | `oi_p2_feed` | `OI_P2_ERR_ARENA_OVERFLOW`; caller (ingest_backend) must drain/rotate before further feeds — no silent frame drop at the ABI level (frame-level loss above is a K1-internal, spec-legal tolerance, not this case) |
| Slot never reaches completeness by drain deadline | `oi_p2_drain` | proceeds with best-effort bitmap (class-(a) structural gate only allows this — P2-R15a); class-(b) oracle-packed pcaps are expected complete, so this path returns `OI_P2_ERR_SLOT_INCOMPLETE` there instead of a TB record |
| K1 vs CPU `uplane_message_decoder` structural mismatch | `oracle_compare.py`, P2.1 gate | CI fail; RE-grid + bitmap diff dumped per-symbol; per D9 this is consistency-vs-reference, not conformance — a mismatch is investigated as a K1 bug first (only known implementation), never dismissed |
| Float-stage tolerance exceeded (K2/K3/K4-input) | `oracle_compare.py`, recorded threshold (Section 7 below) | CI fail with metric value + threshold; **before treating as a port bug**, check DEV-043 (read the oracle/vector generator source — is the vector itself an edge-case shape, e.g. min-length BG?) and the "same-lineage oracle caveat" (HLD D8, SPEC honesty-ledger #6: srsRAN vectors are `release_24_10_1`, OCUDU port is `release_26_04` — verify algorithm parity before blaming the port) |
| Integer-stage (K5/K6) bit mismatch | `oracle_compare.py` | CI fail, zero tolerance; same DEV-043 same-lineage check applies before assuming a port bug |
| LDPC in-pipeline regression | existing bit-exact suite, run inside this pipeline's buffers | CI fail; since P2-R9 requires "no re-quantization between K6 and LDPC," first check is an I6 ABI mismatch (wrong `full_length`/filler placement), not the (already-proven) LDPC kernel itself |
| CRC24A/24B mismatch on oracle-packed pcap | `oi_p2_drain` + integration test | class-(b) hard fail (P2-R15b is the pass/fail decode gate); class-(a) pcaps never assert this (no ground truth exists, DEV-044) — asserting CRC there would be exactly the DEV-044 mistake (trusting a synthetic-data path as ground truth) |
| AGPL artifact detected in shippable output | `agpl_denylist.py`, CI packaging step | build fails; names the offending path; srsRAN vectors must appear only via the CI bind-mount, never baked into an image/tarball (P2-R13) |
| Missing provenance entry | `provenance_check.py`, CI | build fails; names the kernel source file lacking a `provenance.json` entry (P2-R12) |

## 7. Test plan (per requirement)

Per-kernel harnesses run dual-oracle per P2-R14: srsRAN `release_24_10_1` AGPL vectors from the
local checkout (CI-only, never packaged) + Sionna-generated vectors (Apache-2.0, shippable).
**AMENDED 2026-07-23**: as actually built (K1/K5/K6/K2/K3/K4, p2f-integration), "dual-oracle" means
comparing against the real, currently-linked OCUDU library itself — neither the srsRAN-AGPL vector
pipeline nor the Sionna vector pipeline was built; see P2-R14's SPEC.md entry for the adopted
rationale. Float-stage tolerance metric: **normalized RMSE** (`||x_test - x_oracle|| / ||x_oracle||`)
unless noted. Per DEV-044: none of these tests may treat a benchmark/emulator's *default* synthetic
output as ground truth — class-(a) pcaps (P1-captured, ru_emulator static IQ) are explicitly
gated as structural-only (no TB assertion) for this exact reason; ground truth comes only from the
oracle vector generators and the oracle-packed pcaps (class b), whose *own* generation code is
read (DEV-043) before any "odd" vector is assumed wrong.

| Req | Test |
|---|---|
| P2-R1 | Per sub-phase CI stage builds/tests the prefix pipeline (K1-only, K1-K2, ... K1-K6+LDPC); `tests/integration/pipeline_test.py` parametrized by prefix length; each prefix must build and its tap (I2..I6) must be readable even with downstream kernels absent (dummy sink). |
| P2-R2 | `lint_portability.py`: static grep over all `.cl`/SYCL sources for `warpSize`, `__shfl`, inline `asm`, vendor-specific `#ifdef __NVPTX__`/`__AMDGCN__` etc. outside `oi_kernel_compat.h`; `get_kernel_work_group_info` used, no literal work-group-size constants in kernel source. Dynamic: PoCL run every commit, Oclgrind nightly (P2-R2 gate table). |
| P2-R3 | `tests/unit/k1_test.py`: RE grid + bitmap byte-compared (after wire->fp32) against OCUDU CPU `uplane_message_decoder` replay on the same frames, both pcap classes (a)+(b); negative cases: dropped frame (bitmap bit unset, grid entries zero), duplicated frame (idempotent), reordered frames (order-independent scatter) — see error table. |
| P2-R4 | `tests/unit/k2_test.py`: chan-est vs srsRAN vectors (CI) + Sionna (shippable), NRMSE <= threshold T_K2 (recorded below); per-MCS (Qm-independent stage, run once per numerology config not per MCS). |
| P2-R5 | `tests/unit/k3_test.py`: eq symbols + noise vars vs both oracles, NRMSE <= T_K3; noise-var check uses relative error (noise floor near zero make NRMSE ill-conditioned) — recorded metric variant noted alongside T_K3. |
| P2-R6 | `tests/unit/k4_test.py`: feed identical float (eq_symbols, eq_noise_var) from the oracle to K4; int8 LLR output bit-exact (0 mismatches) vs both oracles, for all three MCS (Qm 2/4/6); saturation-boundary vectors (values right at +-range_limit) included explicitly. |
| P2-R7 | `tests/unit/k5_test.py`: Gold sequence x1/x2 state + sign-flipped LLRs bit-exact vs `apply_xor` reference, all three MCS, both oracles; boundary case: input LLR = +-LLR_INFTY passes through sign-flipped, never reinterpreted as a finite value. |
| P2-R8 | `tests/unit/k6_test.py`: bit-exact vs both oracles, BG1+BG2, all three MCS, rv=0 only; filler-bit positions checked == +LLR_INFTY exactly; the min-length/max-length BG corner configs (DEV-043 pattern) included as explicit fixtures, with the vector-generator source read first if either looks anomalous. |
| P2-R9 | Existing LDPC bit-exact suite re-run with this pipeline's I6 buffers as input (not its own synthetic harness), 0/N mismatches; separately, an ABI check confirms no re-quantization step exists between K6's `cb_llr_out` and the LDPC kernel's input pointer (same `int8` buffer, no copy-and-rescale). |
| P2-R10 | `oi_p2_cb_segment` unit test: CB segmentation params (K_cb, C, Z_c, filler count) vs TS 38.212 §5.2.2 worked examples for all three MCS' TB sizes; CRC24B per-CB and CRC24A-over-TB compared bit-exact against `crc_calculator_generic_impl`-equivalent reference for known messages. |
| P2-R11 | Config validator unit tests: the exact MVP YAML accepted; each single-field perturbation (numerology, bandwidth, nof_layers, mcs outside {4,13,21}, rv!=0, uci_on_pusch!=none, compression!=uncompressed_16bit) individually rejected with the specific offending key named. |
| P2-R12 | `provenance_check.py`: every `k*.cl` source has a `provenance.json` entry with repo URL, `release_26_04` tag, clone SHA `4e9f8d6...`, and per-file port source path(s); CI fails on any kernel source missing an entry. |
| P2-R13 | `agpl_denylist.py`: scans built images/tarballs for any path under `third_party/srsRAN_Project`; scans the repo itself (not just images) for vendored srsRAN test-data headers; srsRAN vectors reachable only via the CI-time bind mount path in Section 5's config. |
| P2-R14 | **AMENDED 2026-07-23 (adopted interpretation, see SPEC.md P2-R14):** every kernel test asserts against the real, currently-linked OCUDU library (not static srsRAN-AGPL/Sionna vector files) — satisfied by K1/K5/K6/K2/K3/K4's actual test suites (`tests/k1_test.cpp` etc., not the `tests/unit/k*_test.py` names this row originally specified). Float kernels record NRMSE/relative-error against this oracle (LLD §7's threshold table); integer kernels (K5/K6) are bit-exact against it, no tolerance column. The original srsRAN-vector "verified applicable to OCUDU 26.04" flag / P2-R14a WARN-not-CI-red mechanism was never built since no srsRAN-vector pipeline exists to apply it to. |
| P2-R15 | `tests/integration/pipeline_test.py`: class (a) P1-captured pcaps -> structural gate only (K1 bitmap/grid match CPU decoder replay, pipeline runs to completion, stable outputs across repeat runs — no CRC/TB assertion, DEV-044 rationale documented inline in the test); class (b) oracle-packed pcaps -> CRC24A pass + TB bit-exact vs the packer's own known oracle TB, for all three MCS. |
| P2-R16 | `lint_no_perf.sh` (shared pattern with p1/p0): grep feature tree for threshold-bearing keys (`*_max_us`, `latency`, `throughput`, timing asserts) in gating code paths -> zero hits; kernel-time printouts (if any) must appear only as unasserted debug/log lines, checked by the same lint excluding `debug`/`log`-tagged lines. |
| P2-R17 | API-surface test: p3's live-tap-ul-inject stub and p4's seam stub (both built as thin fakes in this test) link against `oi_p2_host.h` unchanged; a synthetic "swap pcap feed for live feed" test calls only `oi_p2_feed`/`oi_p2_launch_slot`/`oi_p2_drain` and asserts no kernel recompilation or API-signature change is needed between the two feed sources. |

Gate mapping (SPEC "Acceptance gates" table copied for traceability): P2.1={R3}, P2.2={R4},
P2.3={R5}, P2.4={R6}, P2.5={R7}, P2.6={R8}, LDPC-dep={R9}; integration/P2 exit={R1,R15,R16,R17};
cross-cutting (every sub-phase)={R2,R11,R12,R13,R14}.

### Recorded float-kernel tolerances (P2-R14, measured 2026-07-22, p2d-k2-k3)

| Kernel | Metric | Threshold | Status |
|---|---|---|---|
| K2 (chan-est) | NRMSE vs real OCUDU `dmrs_pusch_estimator`, complex-valued | **T_K2 = 0.05** (5%) — measured max 0.0226 across a 3-point SNR sweep (0.005σ-0.15σ noise), margin ~2.2x | **set** (§8 Q1 resolved) |
| K2 (noise_var) | relative error vs real oracle, scalar | **T_K2n = 0.05** (5%) — measured max 0.0236 across the same sweep, margin ~2.1x. Not in this table's original scope (only K2/K3's ch_est/eq outputs were listed) — added here since K2 genuinely outputs it and K3 consumes it as an input. | **set** (new row, added by p2d-k2-k3) |
| K3 (eq symbols) | NRMSE vs real OCUDU `channel_equalizer_generic_impl` (mmse=zf path), complex-valued | **T_K3 = 0.005** (0.5%) — measured max 0.0000985 across 9 (noise_var, tx_scaling) combinations, margin ~50x. The dominant real error source is identified (OCUDU's SIMD fast path uses an approximate reciprocal instruction, `ocudu_simd_f_rcp`; this kernel does exact division and is arguably more precise) — the generous margin accounts for that gap being a real, understood, non-bug difference, not slack for undetected errors. | **set** (§8 Q1 resolved) |
| K3 (post-eq noise var) | relative error vs real oracle, scalar | **T_K3n = 0.005** (0.5%) — measured max 0.00055, same margin rationale as T_K3. | **set** (§8 Q1 resolved) |
| K4 (float LLR pre-quantization, diagnostic only) | not gated — K4's gate is the bit-exact int8 output | n/a | n/a |
| K2 (epre) | relative error vs real oracle, scalar | not formally gated (P2-R4's wording only gates ch_est's NRMSE; epre has no declared downstream consumer in this LLD's I3 usage) | diagnostic only — measured max 0.00015 across the sweep, listed for completeness |

Thresholds are recorded here as the single source of truth once measured; this table is
append-only evidence, not a design decision to defer. Full measurement methodology (test configs,
raw per-config numbers, oracle construction) in `p2d-k2-k3/VERIFICATION.md` and
`p2d-k2-k3/tests/k2_test.cpp`/`k3_test.cpp` (the sweep is part of the committed test, reproducible
via `make test`, not a one-off measurement).

## 8. Open questions

1. **Q1 — RESOLVED (2026-07-22, p2d-k2-k3).** T_K2/T_K2n/T_K3/T_K3n measured and recorded in §7's
   table via a 3-point SNR sweep against the real linked OCUDU `dmrs_pusch_estimator`/
   `channel_equalizer_generic_impl` (not the srsRAN-AGPL/Sionna dual-oracle pair the original gate
   wording anticipated — see p2d-k2-k3's README for why that substitution was made and what's
   still open about it). All four measured values landed with wide (2x-50x) margin below their
   set thresholds; K3's residual error has an identified, understood source (OCUDU's SIMD
   approximate-reciprocal instruction) rather than being unexplained slack.
2. **Q2 — `oi_frame_desc` byte layout (Section 4.1) is this LLD's own design**, since K1 is T4
   fresh with no OCUDU struct to port verbatim (only field *names/semantics* come from
   `uplane_message_params`/`uplane_section_params`). It is not yet cross-checked against the exact
   byte offsets ru_emulator's socket transceiver writes on the wire (p1's capture) or against
   whatever p3's live-tap producer will emit — first integration test against real P1 pcaps should
   confirm or force a revision of this table before it's treated as frozen ABI.
3. **Q3 — `oi_p2_tb_record`'s per-field byte widths (Section 4.7) are provisional.** TB size can
   exceed 16 bits at higher MCS/PRB combinations in general 5G NR, but is bounded and computable
   for this MVP's fixed 51-PRB/MCS-{4,13,21} set — the 4-byte `tb_size_bytes` field is generous by
   design; the record's overall byte-precision should be re-verified once `oi_p2_cb_segment`'s
   TS 38.212 §5.2.2 arithmetic is implemented and the exact three TB sizes are known.
4. **Q4 — Slot-timeout value for `oi_p2_launch_slot`/drain-deadline (error table, "Slot never
   reaches completeness")** is not pinned by SPEC/HLD (no perf numbers allowed, P2-R16) — needs a
   non-perf-framed rule (e.g. "N consecutive missing symbols after the last frame of the slot
   arrives" rather than a wall-clock bound) decided at implementation, since a wall-clock timeout
   would itself brush against the no-perf-threshold rule if misread as a latency requirement.
5. **Q5 — DC subcarrier / transform-precoding fields** (`dc_position` in
   `pusch_demodulator_impl::get_ch_data_estimates`) are out of scope (SPEC "Out of scope": no
   transform precoding) but the port source's interfaces carry an `optional<unsigned> dc_position`
   throughout K2/K3's real OCUDU equivalents; this LLD's K2/K3 prototypes omit it entirely on the
   assumption MVP's full-band mapping-type-A allocation has no DC gap to skip. Confirm at
   implementation against `dmrs_pusch_estimator_results` behavior for this exact allocation.
6. **Q6 — Oclgrind coverage of K1's atomic-OR bitmap scatter** (a K1-specific race-detection
   concern not present in the T3-ported kernels, which have no atomics) is not itemized separately
   in SPEC's Oclgrind gate; recommend the nightly job's report explicitly names K1's atomic paths
   so a race there isn't lost among five kernels that don't use atomics at all.
