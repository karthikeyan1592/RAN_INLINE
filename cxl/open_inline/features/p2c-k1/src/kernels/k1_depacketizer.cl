/* k1_depacketizer.cl — K1, depacketizer + RE-grid reassembly (T4 fresh, P2-R3).
 *
 * No port grounding (T4: no CPU-to-GPU equivalent exists anywhere, use_case_classification.md
 * §0.2). Field semantics consulted read-only from OCUDU's
 * include/ocudu/ofh/serdes/ofh_uplane_message_decoder_properties.h /
 * ofh_uplane_message_properties.h / ofh_message_properties.h (structs only, never their
 * implementation) plus the real wire byte offsets derived in oi_oran_wire_layout.h (p2a-scaffold,
 * resolving parent LLD Open Question Q2 for the eCPRI/O-RAN layers — see that header's derivation
 * comment for exact OCUDU source citations; the Ethernet-layer VLAN question resolved 2026-07-24,
 * see oi_oran_wire_layout.h's own header note and p2a/p2c VERIFICATION.md).
 *
 * By the time a descriptor reaches this kernel, oi_oran_preparse() (p2a, called by the
 * ingest_backend — p2a-scaffold/VERIFICATION.md's ABI reconciliation) has already parsed
 * symbol_id/section_id/filter_index/start_prb/nof_prbs/eth_hdr_len into oi_frame_desc. This kernel
 * does NOT re-parse those fields (in particular, it does NOT re-detect the VLAN tag) — it only
 * locates and converts the IQ payload, using OI_WIRE_TOTAL_HEADER_BYTES(desc.eth_hdr_len), the
 * SAME parameterized macro oi_oran_preparse.cpp uses (so a drift between the two would show up as
 * a K1 test failure, not a silent inconsistency).
 *
 * Work-item = one descriptor (== one U-plane section's RE span; MVP: always the whole 51-PRB band
 * in one section, so one work-item scatters all 612 REs of its symbol).
 */
#include "oi_kernel_compat.h"
#include "oi_frame_desc.h"
#include "oi_oran_wire_layout.h"

#pragma OPENCL EXTENSION cl_khr_global_int32_extended_atomics : enable

#define OI_K1_NOF_SYMBOLS 14u
#define OI_K1_NOF_SUBCARRIERS 612u

// Reads one big-endian int16 at byte offset `off`, sign-extends it, and converts to float.
//
// CORRECTED vs parent LLD §4.1's literal wording (found + fixed during p2c-k1, not silently
// applied): the LLD states "float(be16_to_native(x)) / 32768.0f" (a clean Q15/power-of-2
// convention), but OCUDU's real quantizer (lib/ofh/compression/quantizer.h) uses
// gain = (1 << (bit_width-1)) - 1.0f = 32767.0f for bit_width=16 -- one less than the power-of-2
// value, "max positive int16" convention rather than Q15. Confirmed empirically in
// tests/k1_test.cpp: values built with the REAL OCUDU encoder and decoded with the REAL OCUDU
// decoder round-trip through gain=32767, not 32768. Using 32768 here would silently fail the
// P2-R3 "integer-exact after fixed-point->float conversion" gate against that real oracle, and
// -- more importantly -- would be inconsistent with K4's own RANGE_LIMIT_FLOAT constants (parent
// LLD §4.5), which are themselves sourced from real OCUDU demodulation code calibrated against
// this SAME 32767 gain convention. Parent LLD §4.1 should be corrected to match (flagged, not
// silently left wrong).
inline float oi_k1_read_q15(__global const uchar* base, uint off) {
  ushort raw = (ushort)(((ushort)base[off] << 8) | (ushort)base[off + 1]);
  short sval = (short)raw;  // reinterpret bit pattern as two's-complement signed (SIM-tier target)
  return ((float)sval) / 32767.0f;
}

__kernel void k1_depacketize(
    __global const uchar*        arena,
    __global const oi_frame_desc* descs,
    uint                          nof_descs,
    uint                          slot_id,
    __global float2*              re_grid,
    __global uint*                symbol_bitmap) {
  uint gid = get_global_id(0);
  if (gid >= nof_descs) {
    return;
  }

  oi_frame_desc desc = descs[gid];
  if (desc.slot_id != slot_id) {
    return;  // not this slot's descriptor -- another work-item (or a later launch) owns it
  }

  // Per-descriptor bounds check (LLD error table row 1): declared nof_prbs must fit within the
  // frame's actual byte length, header included. Malformed/truncated -> drop, not fatal: leave
  // the bitmap bit unset and don't scatter anything for this descriptor.
  //
  // desc.eth_hdr_len (2026-07-24): the Ethernet header length (14 untagged, 18 with one 802.1Q
  // tag) as already determined by oi_oran_preparse_frame() from the real EtherType position --
  // this kernel does NOT re-detect the tag itself (single source of truth: one parser decides,
  // the kernel obeys, so the two can never disagree -- same reasoning as the oi_p2_feed
  // reconciliation, p2a-scaffold/VERIFICATION.md). p1's ru_emulator finding (--vlan_tag is
  // CLI::Range(1,65536), no untagged option) means the real wire will always carry a tag, but the
  // kernel makes no assumption either way -- it just uses whatever the descriptor says.
  uint total_header_bytes = OI_WIRE_TOTAL_HEADER_BYTES((uint)desc.eth_hdr_len);
  uint required_bytes = total_header_bytes +
                        (uint)desc.nof_prbs * OI_WIRE_RES_PER_PRB * OI_WIRE_BYTES_PER_RE;
  if (desc.frame_len < required_bytes || desc.symbol_id >= OI_K1_NOF_SYMBOLS) {
    return;
  }

  __global const uchar* iq_base = arena + desc.arena_offset + total_header_bytes;
  uint nof_res = (uint)desc.nof_prbs * OI_WIRE_RES_PER_PRB;
  uint subcarrier0 = (uint)desc.start_prb * OI_WIRE_RES_PER_PRB;

  for (uint re = 0; re < nof_res; re++) {
    uint subcarrier = subcarrier0 + re;
    if (subcarrier >= OI_K1_NOF_SUBCARRIERS) {
      break;  // defensive: a malformed start_prb/nof_prbs combination must not write out of bounds
    }
    uint byte_off = re * OI_WIRE_BYTES_PER_RE;
    float i_val = oi_k1_read_q15(iq_base, byte_off);
    float q_val = oi_k1_read_q15(iq_base, byte_off + 2);
    // Plain (non-atomic) store: duplicate-frame races on the same RE are an accepted SIM-tier
    // tolerance (LLD error table: "second write wins, last-write, not summed" — this is a WRITE,
    // not a read-modify-write, so no lost-update hazard the way the bitmap OR below would have).
    re_grid[desc.symbol_id * OI_K1_NOF_SUBCARRIERS + subcarrier] = (float2)(i_val, q_val);
  }

  // Atomic OR: multiple work-items (duplicate/retransmitted sections for the same symbol) may set
  // the same bit concurrently -- a plain read-modify-write |= would be a genuine lost-update race
  // (LLD §8 Open Question Q6 names this exact path for Oclgrind race-detection coverage).
  atom_or(symbol_bitmap, (uint)(1u << desc.symbol_id));
}
