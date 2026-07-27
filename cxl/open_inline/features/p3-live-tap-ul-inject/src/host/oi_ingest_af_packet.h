/* oi_ingest_af_packet.h — M3: gpu-phy's SIM ingest_backend (LLD §Public APIs). af_packet tap +
 * bounded O-RAN header pre-parse (via the shared p2a-scaffold oi_oran_preparse_frame helper) +
 * delivery into the real p2 pipeline via oi_p2_feed. Purely passive (P3-R8): never transmits.
 *
 * Two real, disclosed deviations from the LLD's literal API text (LLD §Public APIs), both
 * necessary and neither breaking anything already frozen (oi_ingest is p3's OWN API surface, not
 * an already-shipped cross-feature contract like oi_p2_host.h):
 *
 * 1. LLD shows `oi_ingest_handle oi_ingest_open_af_packet(const char* iface);` and
 *    `oi_ingest_counters oi_ingest_poll(oi_ingest_handle h);` with no pipeline pointer anywhere,
 *    yet poll's own doc comment requires calling `oi_p2_feed(pipeline, &desc)`. Genuinely
 *    underspecified, not silently guessed: resolved by adding the pipeline pointer (plus the
 *    arena's total byte budget, needed for #2 below) as construction-time parameters, stored in
 *    the opaque handle -- consistent with the LLD's own "own one oi_oran_preparse_state per
 *    stream" ownership pattern already established for this same handle.
 * 2. Continuous multi-slot demux needs the arena to be used as a genuine ring buffer (a single
 *    bounded run, like pipeline_runner.cpp's one-shot feed loop, is explicitly NOT what this
 *    module does -- see the session brief's own "continuous multi-slot demux (the slot-bound
 *    TODO pipeline_runner.cpp deliberately left to p3)"). oi_ingest_poll wraps arena_offset back
 *    to 0 whenever the next frame wouldn't fit before the configured arena_bytes budget, never
 *    splitting a frame's bytes across the wrap boundary. This module does NOT call
 *    oi_p2_launch_slot/oi_p2_drain itself (LLD's M3 text only ever describes feed()); driving
 *    those per completed slot is the harness's (M5's) job, which is why oi_ingest_last_slot_id()
 *    (additive, not in the LLD's oi_ingest_counters struct) exists below -- M5 polls it to detect
 *    a slot boundary (the running_slot_id oi_oran_preparse_frame already tracks internally) and
 *    knows when to call launch_slot+drain for the slot that just finished.
 */
#ifndef OI_INGEST_AF_PACKET_H
#define OI_INGEST_AF_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "../../../p2a-scaffold/src/host/oi_p2_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oi_ingest_state* oi_ingest_handle;

// LLD §Public APIs, byte-for-byte as specified.
typedef struct {
  uint64_t frames_seen;         // ETH_P_ALL total before the kernel filter
  // Real, disclosed rename-in-place (2026-07-26, found live on GCP): this counter used to mean
  // "ethertype==0xAEFE matched" only. On a real bridge with a 3rd promiscuous listener (gpu-phy is
  // not the intended L2 destination of UL traffic, so DU-bound DL frames only become visible to it
  // via hub-mode flooding, ageing_time=0), that definition let ~80% irrelevant DL traffic reach
  // userspace and cost a real, measured 2:1 system/user CPU-time penalty for zero benefit -- the
  // BPF filter now ALSO requires src MAC == the configured RU MAC (P3-R9's own accounting depends
  // on "tap count == ru-emu's real UL TX counter", which only holds once DL is excluded in-kernel,
  // not just ethertype-matched). Kept the same field name (not a new counter) since it is still
  // exactly "count of frames the kernel filter accepted" -- only the filter's own definition grew
  // a second condition; every existing caller checking `ethertype_matched > 0` /
  // `delivered + parse_failed + feed_backpressure == ethertype_matched` still holds unchanged.
  uint64_t ethertype_matched;   // post-BPF-filter: ethertype==0xAEFE AND src_mac==configured RU MAC
  uint64_t parse_failed;        // filter-matched but common/section-header pre-parse rejected it
  uint64_t delivered;           // successfully handed to oi_p2_feed
  uint64_t feed_backpressure;   // oi_p2_feed returned backpressure (frame not delivered)
  uint64_t socket_drops;        // from PACKET_STATISTICS (tp_drops) -- nonzero invalidates the run (P3-R13)
} oi_ingest_counters;

// LLD §Data structures, p3-internal only; never crosses the p2 ABI (P3-R10).
typedef struct {
  uint64_t reap_seq;      // monotonic, matches oi_frame_desc write order
  uint64_t rx_ts_ns;      // CLOCK_MONOTONIC_RAW at reap
  uint32_t arena_offset;  // cross-reference to the fed descriptor
} oi_ingest_rx_log_entry;

// Construction: bind SOCK_RAW/ETH_P_ALL on `iface`, attach the VLAN-aware classic BPF filter
// (LLD's ethertype pattern: ethertype==0xAEFE @ byte 12, OR 0x8100 @ 12 AND 0xAEFE @ 16 --
// EXTENDED 2026-07-26 with a src-MAC==ru_mac check on top, see oi_ingest_counters's own doc
// comment for why), enable PACKET_STATISTICS + PACKET_AUXDATA. `pipeline` must already be
// oi_p2_setup'd by the caller; `arena_bytes` must match the value oi_p2_setup used (this module
// owns no way to query it back from the opaque oi_p2_pipeline*). `ru_mac` is 6 raw bytes (network
// byte order, as in the wire header) -- callers already have this from their own rig config (the
// same `ru_mac_addr` YAML key p1/p3's own configs already carry); this module never hardcodes it,
// matching the config-not-code-constant precedent this project already follows for every other
// pinned MAC. `udcomphdr_bytes` (added 2026-07-26, real bug found live on GCP -- see
// oi_oran_wire_layout.h's header comment): OI_WIRE_UDCOMPHDR_BYTES_ABSENT (0) or _PRESENT (2),
// passed straight through to oi_oran_preparse_frame() -- callers already know which their rig's
// RU emitter uses (the real ru_emulator binary always uses PRESENT/2; this module never guesses).
// Returns NULL on any setup failure (socket/bind/filter).
oi_ingest_handle oi_ingest_open_af_packet(const char* iface, oi_p2_pipeline* pipeline, uint64_t arena_bytes,
                                          const uint8_t ru_mac[6], uint8_t udcomphdr_bytes);

// Reaps as many frames as are queued (non-blocking), in kernel-delivery order. Never transmits
// (P3-R8). Returns a COUNTERS SNAPSHOT (cumulative since open, not since the last poll -- callers
// wanting deltas must snapshot-and-subtract themselves, matching P1's own soak_stability.sh
// pattern of two absolute snapshots rather than a stateful "since last call" API).
oi_ingest_counters oi_ingest_poll(oi_ingest_handle h);

// Additive query (see file header item 2): the slot_id of the most recently successfully-parsed
// frame, i.e. oi_oran_preparse_state's own running_slot_id as of the last poll. Callers (M5) use
// this to detect a slot boundary by watching for a change across successive polls.
uint32_t oi_ingest_last_slot_id(oi_ingest_handle h);
// True once at least one frame has been successfully parsed (oi_ingest_last_slot_id is undefined
// before this is true, matching oi_oran_preparse_state's own has_last_symbol semantics).
int oi_ingest_has_last_slot_id(oi_ingest_handle h);

// P3-R10 verification-tooling access to the internal RX log (M4/M5 only; never read by anything
// in the p2 feed path itself).
size_t oi_ingest_rx_log_size(oi_ingest_handle h);
oi_ingest_rx_log_entry oi_ingest_rx_log_get(oi_ingest_handle h, size_t index);

void oi_ingest_close(oi_ingest_handle h);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OI_INGEST_AF_PACKET_H
