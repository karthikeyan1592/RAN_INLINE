/* oi_osg_schedule.h — P3-R6's slot -> oracle-file schedule formula (LLD §3.2), the ONE canonical
 * definition. Deliberately tiny (a single inline function) because it must exist in TWO places
 * that cannot share a compiled library: this project's harness (M5, links this header directly)
 * and the ru_emulator patch's M2 loader (a different binary, patched OCUDU source tree -- the
 * patch series carries a byte-identical copy of this exact function, see
 * patches/0002-oracle-grid-loader.patch's ru_emulator_oracle_grid.h). P3-R6 requires the harness
 * to compute file_idx "independently... without any shared runtime state" from the patch, which a
 * single compiled library would violate anyway -- two textually-identical, independently-compiled
 * copies is the correct shape here, not a workaround. If this formula ever changes, both copies
 * must be updated together (grep for "P3-R6 schedule formula" to find both).
 */
#ifndef OI_OSG_SCHEDULE_H
#define OI_OSG_SCHEDULE_H

#include <cstdint>

namespace oi_osg {

// P3-R6 schedule formula: file_idx(sfn, slot) = (sfn * slots_per_frame + slot) mod N.
// slots_per_frame for the pinned MVP numerology (mu=1, 30kHz SCS) = 20 (TS 38.211 Table 4.3.2-1).
constexpr unsigned kSlotsPerFrameMu1 = 20;

inline uint64_t osg_schedule_file_idx(uint64_t sfn, uint64_t slot, uint64_t slots_per_frame, uint64_t n_files) {
  return (n_files == 0) ? 0 : (sfn * slots_per_frame + slot) % n_files;
}

}  // namespace oi_osg

#endif  // OI_OSG_SCHEDULE_H
