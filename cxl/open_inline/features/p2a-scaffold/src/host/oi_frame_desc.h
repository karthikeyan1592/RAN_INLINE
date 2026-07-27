/* oi_frame_desc.h — I1 packet-arena descriptor (LLD §4.1), 32 bytes.
 *
 * Byte layout is this LLD's own design (Open Question Q2 — not yet cross-checked against real
 * wire bytes; p2c-k1 will confirm or revise once tested against real p1 pcaps / p3's live-tap
 * producer). Defined HERE (not in p2c-k1) because the arena/descriptor-ring BUFFER MANAGEMENT
 * (sizing, allocation, ring bookkeeping) is p2a's job (HLD §5 buffer-pool ownership).
 *
 * FIELD OWNERSHIP (reconciled 2026-07-22, superseding the earlier "host-filled, unspecified who"
 * framing): every field is filled by the ingest_backend (p3 in SIM, p6 in PHYSICAL) via the
 * shared oi_oran_preparse() helper (oi_oran_preparse.h) BEFORE calling oi_p2_feed() — never
 * inside feed() itself, and never fabricated by the pipeline. See oi_oran_preparse.h for why
 * (parsing must happen where the frame bytes are host-readable, which is backend-specific; PHY
 * ical's dmabuf path in particular cannot cheaply parse from inside the p2 pipeline).
 *
 * Plain C type (not a C++ namespace member) because it crosses the oi_p2_host.h C/C++ boundary:
 * ingest-backend code (which may be plain C, e.g. an af_packet capture loop) constructs one of
 * these directly and passes a pointer to oi_p2_feed().
 *
 * DEVICE-SIDE INCLUDE (found + fixed during p2c-k1, not previously exercised): K1's kernel
 * prototype takes `__global const oi_frame_desc*` (parent LLD §3), meaning this header must also
 * compile as OpenCL C, not just host C/C++. OpenCL C kernel compilers (PoCL included) have no
 * `<stdint.h>` -- it's a freestanding device-C dialect with its own guaranteed-width scalar types
 * (uchar/ushort/uint/ulong), not the host libc. No earlier p2* kernel had actually included this
 * header (K5/K6 only include the empty oi_kernel_compat.h), so this gap was latent until K1 tried
 * to build. `__OPENCL_C_VERSION__` is the standard predefined macro used to pick the device-native
 * types instead.
 */
#ifndef OI_FRAME_DESC_H
#define OI_FRAME_DESC_H

#ifdef __OPENCL_C_VERSION__
typedef uchar oi_u8;
typedef ushort oi_u16;
typedef uint oi_u32;
typedef ulong oi_u64;
#else
#include <stdint.h>
typedef uint8_t oi_u8;
typedef uint16_t oi_u16;
typedef uint32_t oi_u32;
typedef uint64_t oi_u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Exactly 32 bytes (LLD §4.1). Packed so the host-side layout matches the device-side __global
// oi_frame_desc* view byte-for-byte -- no compiler-inserted padding. Every field here is already
// at a naturally-aligned offset (no gaps a standard compiler would introduce anyway), so
// `#pragma pack` is host-side belt-and-suspenders only -- skipped in OpenCL C, where support for
// the pragma isn't guaranteed by the spec and isn't needed for this particular layout.
#ifndef __OPENCL_C_VERSION__
#pragma pack(push, 1)
#endif
typedef struct {
  oi_u64 arena_offset;   // offset  0: byte offset into arena buffer
  oi_u32 frame_len;      // offset  8: raw Ethernet frame length, bytes
  oi_u32 slot_id;        // offset 12: host slot counter (wraps at 2^32; derived by the ingest
                         //            backend from symbol_id wrap detection -- NOT the 3GPP
                         //            slot_point bit-packing, and NOT derivable by feed() itself)
  oi_u8  symbol_id;      // offset 16: 0-13, from O-RAN U-plane section header
  oi_u8  section_id;     // offset 17: from U-plane section header
  oi_u8  filter_index;   // offset 18: from common header; MVP expects one fixed value
  oi_u8  flags;          // offset 19: bit0 is_every_rb_used, bit1 use_current_symbol_number
  oi_u16 start_prb;      // offset 20: 0-50 (MVP: always 0)
  oi_u16 nof_prbs;       // offset 22: 1-51 (MVP: always 51)
  oi_u8  eth_hdr_len;    // offset 24: 14 (untagged) or 18 (one 802.1Q tag) -- the reserved-bytes
                         //            "future VLAN field" this struct's own comment already
                         //            anticipated. Set by oi_oran_preparse_frame() from the real
                         //            EtherType position (byte 12, or byte 16 if 0x8100 sits at
                         //            12); K1's kernel reads this instead of re-parsing the tag
                         //            itself, so the two can never disagree (2026-07-24, p1's
                         //            ru_emulator finding: --vlan_tag is CLI::Range(1,65536), no
                         //            untagged option, so the real wire always carries a tag).
  oi_u8  payload_byte_off; // offset 25: absolute byte offset from frame start where IQ payload
                         //            begins -- eth_hdr_len + eCPRI header + O-RAN msg/section
                         //            header + udCompHdr/reserved (0 or 2 bytes, see
                         //            oi_oran_wire_layout.h's OI_WIRE_UDCOMPHDR_BYTES_* constants).
                         //            Set by oi_oran_preparse_frame() from the caller-supplied
                         //            udcomphdr_bytes parameter (2026-07-26, real bug found live on
                         //            GCP: real captured UL frames from ru_emulator's own frame
                         //            builder carry a 2-byte udCompHdr+reserved field that
                         //            OI_WIRE_TOTAL_HEADER_BYTES(eth_hdr_len) alone doesn't account
                         //            for -- see oi_oran_wire_layout.h's header comment for the
                         //            full citation trail). Same "one parser decides, kernel obeys"
                         //            precedent as eth_hdr_len -- K1's kernel reads this field
                         //            directly instead of re-deriving compression mode on-device.
  oi_u8  reserved[6];    // offset 26: zero-filled; future eAxC/BFP fields land here
} oi_frame_desc;
#ifndef __OPENCL_C_VERSION__
#pragma pack(pop)
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#define OI_FRAME_DESC_FLAG_EVERY_RB_USED 0x01u
#define OI_FRAME_DESC_FLAG_USE_CURRENT_SYMBOL_NUMBER 0x02u

#ifdef __cplusplus
static_assert(sizeof(oi_frame_desc) == 32, "oi_frame_desc must be exactly 32 bytes (LLD §4.1)");
#endif

#endif /* OI_FRAME_DESC_H */
