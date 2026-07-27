// oi_ingest_af_packet.cpp — see oi_ingest_af_packet.h for the API/deviation rationale.
//
// BPF filter grounding: hand-assembled classic BPF (cBPF, SO_ATTACH_FILTER), not eBPF -- af_packet
// socket filters use the classic instruction set (<linux/filter.h>). Encodes the LLD §Public APIs
// ethertype pattern:
//   (ethertype @ 12 == 0xAEFE) OR (ethertype @ 12 == 0x8100 AND ethertype @ 16 == 0xAEFE)
// PLUS (added 2026-07-26, real bug found live on GCP -- see oi_ingest_counters's doc comment in
// the header for the full story) a src-MAC check: bytes 6-11 (the Ethernet source address, same
// offset regardless of VLAN tag presence) must equal the configured RU MAC. Without this, gpu-phy
// -- a 3rd promiscuous listener on the fronthaul bridge, not the frames' real destination -- sees
// every DL frame too once the bridge is in hub mode (ageing_time=0, needed for UL visibility in
// the first place), at ~4x the volume of the UL traffic it actually needs: measured live, this
// cost a real 2:1 system/user CPU-time penalty (kernel-to-userspace copy + syscall overhead for
// frames immediately discarded downstream) with zero benefit, since gpu-phy only ever wants UL.
// Verified independently two ways in this feature's test suite (ingest_af_packet_test.cpp): (a) a
// pure-C reimplementation of the same decision logic cross-checked against real captured corpus
// fragments and synthetic frames, and (b) the REAL attached kernel filter exercised end-to-end over
// a real veth pair against the FULL real captured corpus (840,783 real frames, both directions,
// known ground-truth split), both required to agree.
#include "oi_ingest_af_packet.h"

#include "../../../p2a-scaffold/src/host/oi_oran_preparse.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

struct oi_ingest_state {
  int sock_fd = -1;       // BPF-filtered: the real delivery path (P3-R7's filter requirement)
  int count_sock_fd = -1; // UNFILTERED companion, same iface, ETH_P_ALL -- see file header
                          // "frames_seen vs ethertype_matched" note: with a real kernel-level
                          // SO_ATTACH_FILTER, a `ret 0` reject drops the frame before it EVER
                          // reaches userspace or increments that socket's own PACKET_STATISTICS
                          // tp_packets counter -- there is no way for a single filtered socket to
                          // observe pre-filter traffic. This second, filter-less socket exists
                          // solely so frames_seen can be a REAL pre-filter count (via its own
                          // tp_packets) rather than degenerately always equaling
                          // ethertype_matched. Never recvmsg'd from -- its own kernel receive
                          // buffer is left to drop once full for a long-running session (its
                          // tp_drops is not consulted; only tp_packets, which increments before
                          // buffer-full drops do), which is fine for this feature's bounded test/
                          // verification runs.
  oi_p2_pipeline* pipeline = nullptr;
  uint64_t arena_bytes = 0;
  uint64_t arena_write_offset = 0;  // ring-buffer cursor (see header item 2)
  uint8_t udcomphdr_bytes = 0;      // OI_WIRE_UDCOMPHDR_BYTES_ABSENT/_PRESENT, caller-supplied
                                    // (2026-07-26 fix, see oi_oran_wire_layout.h)

  oi_oran_preparse_state preparse_state{};

  oi_ingest_counters counters{};
  uint64_t reap_seq_next = 0;
  std::vector<oi_ingest_rx_log_entry> rx_log;
};

namespace {

uint64_t monotonic_raw_ns() {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Attaches the real, kernel-enforced classic BPF filter: the LLD's two-branch VLAN-aware
// ethertype pattern, PLUS a src-MAC==ru_mac check gating the final accept (see this file's header
// comment for why). Instruction-index derivation, jt/jf are relative to the NEXT instruction:
//
//   (0) ldh [12]                A = ethertype @ 12
//   (1) jeq #0xaefe  jt=3->(5)  jf=0->(2)      untagged match -> skip straight to MAC check
//   (2) jeq #0x8100  jt=0->(3)  jf=7->(10)     not VLAN either -> REJECT
//   (3) ldh [16]                A = ethertype @ 16 (only reached if @12==0x8100)
//   (4) jeq #0xaefe  jt=0->(5)  jf=5->(10)     tagged mismatch -> REJECT
//   (5) ld  [6]                 A = src_mac[0:4] as one big-endian u32 (bytes 6-9)
//   (6) jeq #ru_hi32 jt=0->(7)  jf=3->(10)     first 4 src-MAC bytes mismatch -> REJECT
//   (7) ldh [10]                A = src_mac[4:6] as u16 (bytes 10-11)
//   (8) jeq #ru_lo16 jt=0->(9)  jf=1->(10)     last 2 src-MAC bytes mismatch -> REJECT
//   (9) MATCH:  ret #0xffff (accept, full frame)
//   (10) REJECT: ret #0
//
// Src MAC is always at bytes 6-11 of the Ethernet header regardless of VLAN tag presence, so one
// shared MAC-check tail serves both the untagged and tagged ethertype branches.
bool attach_bpf_filter(int fd, const uint8_t ru_mac[6]) {
  uint32_t ru_hi32 = ((uint32_t)ru_mac[0] << 24) | ((uint32_t)ru_mac[1] << 16) |
                     ((uint32_t)ru_mac[2] << 8) | (uint32_t)ru_mac[3];
  uint32_t ru_lo16 = ((uint32_t)ru_mac[4] << 8) | (uint32_t)ru_mac[5];

  struct sock_filter code[] = {
      {0x28, 0, 0, 0x0000000c},  // (0) ldh [12]                     -- A = ethertype @ 12
      {0x15, 3, 0, 0x0000aefe},  // (1) jeq #0xaefe, jt->(5) MAC check
      {0x15, 0, 7, 0x00008100},  // (2) jeq #0x8100, jf->(10) REJECT
      {0x28, 0, 0, 0x00000010},  // (3) ldh [16]                     -- A = ethertype @ 16
      {0x15, 0, 5, 0x0000aefe},  // (4) jeq #0xaefe, jf->(10) REJECT
      {0x20, 0, 0, 0x00000006},  // (5) ld  [6]                      -- A = src_mac[0:4] (u32 BE)
      {0x15, 0, 3, ru_hi32},     // (6) jeq #ru_hi32, jf->(10) REJECT
      {0x28, 0, 0, 0x0000000a},  // (7) ldh [10]                     -- A = src_mac[4:6] (u16)
      {0x15, 0, 1, ru_lo16},     // (8) jeq #ru_lo16, jf->(10) REJECT
      {0x06, 0, 0, 0x0000ffff},  // (9) MATCH: ret #0xffff (accept, full frame)
      {0x06, 0, 0, 0x00000000},  // (10) REJECT: ret #0
  };
  struct sock_fprog prog{};
  prog.len = (unsigned short)(sizeof(code) / sizeof(code[0]));
  prog.filter = code;
  return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) == 0;
}

// Creates+binds a plain SOCK_RAW/ETH_P_ALL socket on `iface`, no filter attached. Returns -1 on
// any failure. Shared by both the delivery socket and the unfiltered counting-only companion.
//
// SO_RCVBUF sizing (real finding, not a guess, and NOT the first fix attempted -- see below):
// found via this feature's own veth-replay test (P3-U3) -- a burst of 200 real captured frames
// (~2.4KB average) sent back-to-back, then drained by a single poll() after a short sleep,
// produced real nonzero PACKET_STATISTICS tp_drops with the kernel's default receive buffer
// (~208KB on this host, confirmed via `sysctl net.core.rmem_max` = 212992). Plain SO_RCVBUF was
// tried first and had ZERO effect -- root-caused by direct measurement, not assumed: SO_RCVBUF is
// silently capped at net.core.rmem_max regardless of the requested value (208KB < 200 frames *
// ~2.4KB avg ~= 480KB, exactly matching the observed drop count). SO_RCVBUFFORCE (this process
// runs as root / CAP_NET_ADMIN in every environment this project targets, matching the existing
// cap_add: NET_RAW/NET_ADMIN precedent already established for ru-emu's socket transceiver, p1
// HLD D2) bypasses that cap for real, verified by re-running the same test after switching to it
// and confirming socket_drops dropped to 0 (see VERIFICATION.md). This isn't just a test
// artifact: P1's own real measurements showed ~28K frames/sec sustained on a live rig, so an
// under-sized receive buffer is a genuine risk of exactly the "socket_drops invalidates the run"
// failure mode P3-R13 exists to catch. 8MB gives real headroom (>800 max-MTU-9000 frames' worth).
constexpr int kSockRcvBufBytes = 8 * 1024 * 1024;

int open_raw_socket(const char* iface) {
  int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) return -1;

  int rcvbuf = kSockRcvBufBytes;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) != 0) {
    // Not running with CAP_NET_ADMIN (e.g. an unprivileged CI runner) -- fall back to the
    // capped, best-effort request rather than failing open() outright; P3-R13's own
    // nonzero-socket_drops-invalidates-the-run rule is what catches the consequence if this
    // fallback isn't enough for a given run, so this is a soft degradation, not a silent one.
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  struct ifreq ifr{};
  std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_ll sll{};
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = ifr.ifr_ifindex;
  if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

oi_ingest_handle oi_ingest_open_af_packet(const char* iface, oi_p2_pipeline* pipeline, uint64_t arena_bytes,
                                          const uint8_t ru_mac[6], uint8_t udcomphdr_bytes) {
  if (!pipeline || arena_bytes == 0 || !ru_mac) return nullptr;

  int fd = open_raw_socket(iface);
  if (fd < 0) return nullptr;

  if (!attach_bpf_filter(fd, ru_mac)) {
    close(fd);
    return nullptr;
  }

  // PACKET_AUXDATA: surfaces out-of-band VLAN info (TP_STATUS_VLAN_VALID / tp_vlan_tci) for
  // frames whose hardware/driver VLAN offload stripped the inline tag before delivery (LLD's
  // IMPLEMENTATION CAVEAT). This module's handling: if AUXDATA reports a valid VLAN tag on a
  // frame whose raw bytes look untagged (no 0x8100 at byte 12), that is STILL the "simple case"
  // for oi_oran_preparse_frame (it correctly computes eth_hdr_len=14 from the raw bytes) -- no
  // downstream consumer of oi_frame_desc currently needs the VID itself (LLD's own point (a)), so
  // this module enables AUXDATA accounting (so a future consumer could read tp_vlan_tci) but does
  // not reinsert bytes or otherwise change frame handling based on it.
  int auxdata_on = 1;
  setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &auxdata_on, sizeof(auxdata_on));

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return nullptr;
  }

  // Unfiltered companion socket, counting-only (see oi_ingest_state's own comment on why this
  // exists -- a real kernel-level SO_ATTACH_FILTER reject is invisible to a single filtered
  // socket, so frames_seen needs its own, unfiltered observation point).
  int count_fd = open_raw_socket(iface);
  if (count_fd < 0) {
    close(fd);
    return nullptr;
  }

  auto* st = new oi_ingest_state();
  st->sock_fd = fd;
  st->count_sock_fd = count_fd;
  st->pipeline = pipeline;
  st->arena_bytes = arena_bytes;
  st->udcomphdr_bytes = udcomphdr_bytes;
  return st;
}

oi_ingest_counters oi_ingest_poll(oi_ingest_handle h) {
  if (!h) return oi_ingest_counters{};

  uint8_t buf[9000];
  for (;;) {
    struct iovec iov{};
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);

    char cmsg_buf[256];
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(h->sock_fd, &msg, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // queue drained, non-blocking by design
      break;  // any other recv error: stop this poll cycle, counters/state unaffected
    }
    // frames_seen is NOT incremented here -- every recvmsg on this socket has ALREADY passed the
    // kernel-level BPF filter (that's what SO_ATTACH_FILTER means), so this loop only ever sees
    // ethertype-matched frames. frames_seen is sourced separately, from the unfiltered companion
    // socket's own PACKET_STATISTICS, at the end of this function (see that block's comment).

    // PACKET_AUXDATA (LLD IMPLEMENTATION CAVEAT): check for out-of-band VLAN delivery. Per this
    // module's documented handling (header comment), no action needed beyond accounting -- the
    // raw frame bytes (possibly already effectively-untagged if hardware stripped the tag) are
    // exactly what oi_oran_preparse_frame correctly handles either way.
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_PACKET && cmsg->cmsg_type == PACKET_AUXDATA) {
        // tpacket_auxdata inspected only for documentation/parity with the LLD's caveat; no field
        // read here changes ingest behavior (see file/header comments for why).
      }
    }

    h->counters.ethertype_matched++;  // BPF already filtered to only 0xAEFE-matching frames

    // Ring-buffer wraparound (header item 2): never split a frame's bytes across the boundary.
    if (h->arena_write_offset + (uint64_t)n > h->arena_bytes) {
      h->arena_write_offset = 0;
    }
    uint64_t this_offset = h->arena_write_offset;

    oi_p2_status wst = oi_p2_write_arena(h->pipeline, this_offset, buf, (size_t)n);
    if (wst != OI_P2_OK) {
      // Arena write itself failed (e.g. a single frame larger than the whole arena budget) --
      // count as a parse failure class (the frame cannot be delivered either way); do not advance
      // the cursor past a write that didn't happen.
      h->counters.parse_failed++;
      continue;
    }
    h->arena_write_offset = this_offset + (uint64_t)n;

    uint64_t rx_ts = monotonic_raw_ns();

    oi_frame_desc desc{};
    oi_preparse_status pst = oi_oran_preparse_frame(&h->preparse_state, buf, (uint32_t)n, h->udcomphdr_bytes, &desc);
    if (pst != OI_PREPARSE_OK) {
      h->counters.parse_failed++;
      continue;
    }
    desc.arena_offset = this_offset;
    desc.frame_len = (uint32_t)n;

    oi_p2_status fst = oi_p2_feed(h->pipeline, &desc);
    if (fst == OI_P2_ERR_ARENA_OVERFLOW) {
      // Backpressure class (LLD §Error handling: "any nonzero feed_backpressure ... invalidates
      // the bit-exact accounting"). OI_P2_ERR_ARENA_OVERFLOW is oi_p2_feed's own descriptor-ring-
      // full signal (p2-phy-kernels LLD), the closest real status to "feed declined this frame".
      h->counters.feed_backpressure++;
      continue;
    }
    if (fst != OI_P2_OK) {
      h->counters.parse_failed++;
      continue;
    }

    h->counters.delivered++;
    oi_ingest_rx_log_entry entry{};
    entry.reap_seq = h->reap_seq_next++;
    entry.rx_ts_ns = rx_ts;
    entry.arena_offset = (uint32_t)this_offset;
    h->rx_log.push_back(entry);
  }

  // PACKET_STATISTICS: tp_drops accounting (P3-R13) from the REAL delivery socket. struct
  // tpacket_stats has 2 u32 fields (tp_packets, tp_drops); getsockopt resets the kernel-side
  // counter after each read (standard af_packet semantics, LLD §3.3), so this module accumulates
  // into a running total rather than overwriting, matching "cumulative since open" (this file's
  // own poll-semantics doc comment).
  struct tpacket_stats stats{};
  socklen_t stats_len = sizeof(stats);
  if (getsockopt(h->sock_fd, SOL_PACKET, PACKET_STATISTICS, &stats, &stats_len) == 0) {
    h->counters.socket_drops += stats.tp_drops;
  }

  // frames_seen (LLD: "ETH_P_ALL total before ethertype filter") from the UNFILTERED companion
  // socket's own tp_packets -- see oi_ingest_state's comment for why a single filtered socket
  // cannot observe this. tp_packets counts packets successfully queued to THAT socket (i.e.
  // before any application-level drain, but after the kernel's own buffer-full drop point --
  // tp_drops on this companion socket is intentionally never consulted, see header comment).
  struct tpacket_stats count_stats{};
  socklen_t count_stats_len = sizeof(count_stats);
  if (getsockopt(h->count_sock_fd, SOL_PACKET, PACKET_STATISTICS, &count_stats, &count_stats_len) == 0) {
    h->counters.frames_seen += count_stats.tp_packets;
  }

  return h->counters;
}

uint32_t oi_ingest_last_slot_id(oi_ingest_handle h) {
  if (!h) return 0;
  return h->preparse_state.running_slot_id;
}

int oi_ingest_has_last_slot_id(oi_ingest_handle h) {
  if (!h) return 0;
  return h->preparse_state.has_last_symbol;
}

size_t oi_ingest_rx_log_size(oi_ingest_handle h) {
  return h ? h->rx_log.size() : 0;
}

oi_ingest_rx_log_entry oi_ingest_rx_log_get(oi_ingest_handle h, size_t index) {
  if (!h || index >= h->rx_log.size()) return oi_ingest_rx_log_entry{};
  return h->rx_log[index];
}

void oi_ingest_close(oi_ingest_handle h) {
  if (!h) return;
  if (h->sock_fd >= 0) close(h->sock_fd);
  if (h->count_sock_fd >= 0) close(h->count_sock_fd);
  delete h;
}
