# p2c-k1

Implementation slice of [`p2-phy-kernels`](../p2-phy-kernels/spec/) — K1, the depacketizer. The
highest-risk kernel: **T4 fresh**, no CPU-to-GPU port equivalent exists anywhere, public literature
is empty (`use_case_classification.md` §0.2). Isolated into its own slice because its oracle,
error-handling, and validation shape are all qualitatively different from every other kernel here.

## Scope

- **K1 — depacketizer + RE-grid reassembly** (`src/kernels/k1_depacketizer.cl`). Consumes the
  already-parsed `oi_frame_desc` (filled by `oi_oran_preparse`, p2a) and locates/converts the raw
  IQ payload (big-endian int16, `/32767.0f` per the corrected LLD §4.1) into the slot RE grid
  (`float2[14][612]`), emitting a per-symbol completeness bitmap. No code ported (T4, P2-R3); the
  real wire byte offsets it depends on were derived by reading OCUDU's actual decode logic, not by
  porting it.
- `../p2a-scaffold/src/host/oi_oran_wire_layout.h`, `oi_oran_preparse.cpp` (rewritten this slice)
  — Open Question Q2 **resolved for the eCPRI + O-RAN CUS layers**: real bit-packed byte offsets,
  cross-validated against the real OCUDU encoder+decoder in `tests/k1_test.cpp`. Still open: the
  Ethernet II framing assumption (14 bytes, no VLAN) — inferred from p3's af_packet/BPF design,
  not yet confirmed against a real captured pcap. See `VERIFICATION.md`.
- Error-handling per LLD §6: malformed/truncated frame (drop, not fatal), duplicate frame
  (idempotent last-write), reordered frames (order-independent by construction, symbol-indexed
  scatter) — all specifically K1's concerns per the parent error table.
- `tests/k1_test.cpp` — gate is **structural consistency** against OCUDU's own real
  `uplane_message_decoder`/`ecpri::packet_decoder` (D9: no golden vectors exist for a T4 kernel;
  this is consistency-vs-reference, not conformance) — built with, and decoded independently by,
  the REAL OCUDU encoder/decoder, not synthetic frames. **57/57 real assertions pass.** Negative
  cases: dropped/duplicate/reordered frames per the error table, all covered.

## Gates this slice owns

P2.1={R3} from the parent gate-mapping table.

**P2-R14/R14a explicitly does NOT apply to this slice** — stated deliberately, not left silent: K1
is T4 fresh with no golden vectors (neither srsRAN nor Sionna has a "correct RE grid" oracle for
raw eCPRI framing), so there is no dual-oracle comparison to wire. K1's only oracle is the
structural one (D9), already covered under R3 above. If a future revision adds a real vector
source for K1, R14 ownership would move here — until then, its absence from this slice is correct,
not a gap.

## Depends on

`p2a-scaffold` (host API, arena/descriptor-ring plumbing point I1, `oi_kernel_compat.h`).

## Explicitly NOT in this slice

K2–K6, LDPC, pipeline integration. K1 is tested standalone: feed canned frames, read back I2 (RE
grid + bitmap), compare structurally. No dependency on p2b/p2d/p2e.

## Watch item

Open Question Q2 (parent LLD §8) is **resolved for the eCPRI + O-RAN CUS layers** (see
`VERIFICATION.md`), cross-validated against the real OCUDU encoder/decoder — not just derived by
hand. **Still open:** the Ethernet II framing assumption (14 bytes, no VLAN tag) underneath those
layers, inferred from p3's af_packet/BPF design but not yet confirmed against a real captured
pcap. Also newly flagged (not previously known): `oi_frame_desc::section_id` is `uint8_t` but the
real wire field is 12 bits — a latent truncation risk, harmless under MVP's single-section-per-
frame scope, flagged for explicit reconciliation if that scope ever changes (`VERIFICATION.md`).
