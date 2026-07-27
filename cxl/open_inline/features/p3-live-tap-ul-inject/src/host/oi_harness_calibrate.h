/* oi_harness_calibrate.h — M5's slot_id -> file_idx phase calibration.
 *
 * REAL RECONCILIATION FINDING (not previously flagged in the LLD, found while implementing M5,
 * not assumed): p2-phy-kernels' oi_frame_desc.slot_id is a HOST-DERIVED monotonic counter that
 * oi_oran_preparse_frame starts at 0 when a stream's ingest state is constructed and increments
 * on every symbol-wrap (13->0) detected -- confirmed by reading oi_oran_preparse.cpp directly
 * (`state->running_slot_id++`). It has NO built-in relationship to the wire's actual 3GPP
 * (sfn, slot) pair, which is what ru_emulator's OWN internal schedule uses
 * (message_info.symbol_point.get_slot().sfn()/.slot_index(), p3 HLD D1). SPEC.md's own text
 * ("harness derives sfn = slot_id / slots_per_frame") implicitly assumes slot_id equals the
 * wire's real cumulative slot count from a shared (sfn=0, slot=0) origin -- true only if ingest
 * happens to start observing at exactly a slot-0 boundary, which a live tap joining an
 * already-running DU has no guarantee of.
 *
 * Because osg_gen's own real design constraint (see tools/osg_gen.cpp's header) already forces
 * N == slots_per_frame (one oracle file per within-frame slot position, DMRS-safety), file_idx
 * depends ONLY on `slot mod slots_per_frame`, not on sfn at all. So the only unknown is a constant
 * phase offset between the host's running_slot_id and the wire's real slot-within-frame index --
 * constant for the whole run (both counters increment in lockstep once running), recoverable by
 * calibrating against the FIRST successfully decoded TB: try every candidate offset 0..N-1, and
 * the correct one is whichever makes that TB match its candidate oracle file bit-exactly. Lock it
 * in for the rest of the run -- no per-slot search needed afterward.
 */
#ifndef OI_HARNESS_CALIBRATE_H
#define OI_HARNESS_CALIBRATE_H

#include <cstdint>
#include <functional>
#include <optional>

namespace oi_harness {

// file_idx for a given (host slot_id, phase_offset), N == slots_per_frame (osg_gen's invariant).
inline uint32_t file_idx_for(uint64_t slot_id, uint32_t phase_offset, uint32_t slots_per_frame) {
  return (uint32_t)((slot_id + phase_offset) % slots_per_frame);
}

// Calibrates the phase offset from the first decoded TB: `tb_matches_file(candidate_file_idx)`
// must return true iff the given TB record's tb_data is bit-identical to that file's tb_payload
// AND its crc verdict matches (the caller owns the actual byte-compare; this function owns only
// the offset search). Returns nullopt if no candidate offset 0..slots_per_frame-1 matches (a real
// calibration failure -- e.g. a genuine decode bug on the very first slot -- reported as such by
// the caller, not silently defaulted to offset 0).
inline std::optional<uint32_t> calibrate_phase_offset(uint64_t first_slot_id, uint32_t slots_per_frame,
                                                      const std::function<bool(uint32_t)>& tb_matches_file) {
  for (uint32_t offset = 0; offset < slots_per_frame; offset++) {
    uint32_t candidate_file_idx = file_idx_for(first_slot_id, offset, slots_per_frame);
    if (tb_matches_file(candidate_file_idx)) {
      return offset;
    }
  }
  return std::nullopt;
}

}  // namespace oi_harness

#endif  // OI_HARNESS_CALIBRATE_H
