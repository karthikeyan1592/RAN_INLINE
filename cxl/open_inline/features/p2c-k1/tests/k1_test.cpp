// k1_test.cpp — P2-R3 structural-consistency gate (D9: no golden vectors exist for a T4 kernel;
// this is consistency-vs-reference, not bit-exact conformance against an independent oracle
// implementation -- there is only one implementation, OCUDU's own). Builds real wire frames with
// the REAL OCUDU encoder (ecpri::packet_builder + ofh::uplane_message_builder, static/none
// compression), decodes them with the REAL OCUDU decoder as ground truth, and separately with (a)
// our own oi_oran_preparse_frame() and (b) the actual k1_depacketize kernel via PoCL -- comparing
// parsed fields and RE-grid contents. Also exercises K1's error-table tolerances: truncated frame
// (dropped, not fatal), duplicate frame (idempotent, not summed), reordered frames
// (order-independent scatter).
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../../p2a-scaffold/src/host/oi_frame_desc.h"
#include "../../p2a-scaffold/src/host/oi_oran_preparse.h"
#include "../../p2a-scaffold/src/host/oi_oran_wire_layout.h"

// Real OCUDU library (BSD-3, release_26_04) -- the oracle.
#include "ocudu/adt/bf16.h"
#include "ocudu/adt/complex.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/ecpri/ecpri_factories.h"
#include "ocudu/ofh/serdes/ofh_serdes_factories.h"

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

namespace {

using namespace ocudu;      // NOLINT -- test file, matches k5_test.cpp/k6_test.cpp convention
using namespace ocudu::ofh;

constexpr unsigned kNofPrbMvp = 51;
constexpr unsigned kNofSubcarriersMvp = kNofPrbMvp * 12;

// Builds one real, wire-valid frame (Ethernet + eCPRI(real) + O-RAN CUS static/none(real)) for the
// given symbol_id, filled with distinct pseudo-random IQ values. Returns the full frame bytes and,
// via out_iq, the exact cbf16_t values fed to the real encoder (so the test can independently ask
// the real decoder what it thinks those bytes mean).
//
// `vlan_tagged` (2026-07-24, added alongside oi_oran_preparse's VLAN detection -- see
// oi_oran_wire_layout.h's header note and p1's ru_emulator finding that --vlan_tag has no
// untagged option): when true, inserts a real 4-byte 802.1Q tag (TPID 0x8100, TCI=1) between the
// MACs and the EtherType, matching p1-ran-baseline/tools/synth_ecpri_gen.cpp's own tagged-frame
// construction. Default false keeps every existing call site (and its OI_WIRE_ETH_HEADER_BYTES==14
// assumption) unchanged.
std::vector<uint8_t> build_real_frame(uplane_message_builder& uplane_builder,
                                      ocudu::ecpri::packet_builder& ecpri_builder,
                                      uint8_t symbol_id, std::mt19937& rgen,
                                      std::vector<cbf16_t>* out_iq, bool vlan_tagged = false) {
  std::uniform_real_distribution<float> dist(-0.9f, 0.9f);
  out_iq->resize(kNofSubcarriersMvp);
  for (auto& v : *out_iq) {
    v = cbf16_t(dist(rgen), dist(rgen));
  }

  ru_compression_params compr_params{compression_type::none, 16};
  units::bytes oran_header_size = uplane_builder.get_header_size(compr_params);
  std::vector<uint8_t> oran_buf(oran_header_size.value() + kNofSubcarriersMvp * 4, 0);

  uplane_message_params params{};
  params.direction = data_direction::uplink;
  params.slot = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0, 0, 0);
  params.filter_index = filter_index_type::standard_channel_filter;
  params.start_prb = 0;
  params.nof_prb = kNofPrbMvp;
  params.symbol_id = symbol_id;
  params.sect_type = section_type::type_1;
  params.compression_params = compr_params;

  unsigned oran_bytes = uplane_builder.build_message(span<uint8_t>(oran_buf), span<const cbf16_t>(*out_iq), params);

  // WORKAROUND (found by reading ofh_uplane_message_builder_impl.cpp's encode_data_direction()):
  // this builder hardcodes direction=downlink in the wire byte regardless of params.direction --
  // it's meant for O-DU->O-RU (downlink) transmission only; OCUDU's own code never needs to
  // *build* an uplink message, only decode one received from the RU. The decoder we're using as
  // oracle unconditionally rejects non-uplink frames, so flip the one known bit (byte0, bit7)
  // after building -- not a guess, the exact bit position is read verbatim from that function.
  oran_buf[0] &= 0x7Fu;

  units::bytes ecpri_header_size = ecpri_builder.get_header_size(ecpri::message_type::iq_data);
  uint32_t eth_len = vlan_tagged ? OI_WIRE_ETH_HEADER_BYTES_TAGGED : OI_WIRE_ETH_HEADER_BYTES_UNTAGGED;
  std::vector<uint8_t> full(eth_len + ecpri_header_size.value() + oran_bytes, 0);
  std::memcpy(full.data() + eth_len + ecpri_header_size.value(), oran_buf.data(), oran_bytes);

  ecpri::iq_data_parameters iq_params{/*pc_id=*/1, /*seq_id=*/1};
  ecpri_builder.build_data_packet(
      span<uint8_t>(full).subspan(eth_len, ecpri_header_size.value() + oran_bytes), iq_params);

  if (vlan_tagged) {
    full[OI_WIRE_OFF_VLAN_TPID] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q >> 8);
    full[OI_WIRE_OFF_VLAN_TPID + 1] = (uint8_t)(OI_WIRE_VLAN_TPID_8021Q & 0xFFu);
    full[OI_WIRE_OFF_VLAN_TPID + 2] = 0x00;
    full[OI_WIRE_OFF_VLAN_TPID + 3] = 0x01;  // TCI: VID=1, matching p1's fronthaul plan
    full[OI_WIRE_OFF_ETHERTYPE_TAGGED] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN >> 8);
    full[OI_WIRE_OFF_ETHERTYPE_TAGGED + 1] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN & 0xFFu);
  } else {
    full[OI_WIRE_OFF_ETHERTYPE_UNTAGGED] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN >> 8);
    full[OI_WIRE_OFF_ETHERTYPE_UNTAGGED + 1] = (uint8_t)(OI_WIRE_ETHERTYPE_ORAN & 0xFFu);
  }

  return full;
}

}  // namespace

int main() {
  using namespace ocudu;
  using namespace ocudu::ofh;

  ocudulog::init();
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("K1_TEST");
  logger.set_level(ocudulog::basic_levels::debug);

  auto compressor = create_iq_compressor(compression_type::none, logger, 1.0f, "generic");
  auto decompressor = create_iq_decompressor(compression_type::none, logger, "generic");
  check(compressor != nullptr && decompressor != nullptr, "real OCUDU none-compression codec created");

  auto uplane_builder = create_static_compr_method_ofh_user_plane_packet_builder(logger, *compressor);
  auto ecpri_builder = ecpri::create_ecpri_packet_builder();
  check(uplane_builder != nullptr && ecpri_builder != nullptr, "real OCUDU U-plane/eCPRI builders created");

  ru_compression_params compr_params{compression_type::none, 16};
  auto uplane_decoder = create_static_compr_method_ofh_user_plane_packet_decoder(
      logger, subcarrier_spacing::kHz30, cyclic_prefix{}, kNofPrbMvp, /*sector_id=*/0,
      std::move(decompressor), compr_params);
  auto ecpri_decoder = ecpri::create_ecpri_packet_decoder_ignoring_payload_size(logger, /*sector=*/0);
  check(uplane_decoder != nullptr && ecpri_decoder != nullptr, "real OCUDU U-plane/eCPRI decoders created");

  std::mt19937 rgen(9001);

  // --- OpenCL/PoCL setup, build the actual kernel once ---
  cl_platform_id platform = nullptr;
  cl_uint nof_platforms = 0;
  clGetPlatformIDs(1, &platform, &nof_platforms);
  check(nof_platforms >= 1, "OpenCL platform available for kernel test");
  cl_device_id device = nullptr;
  clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr);
  cl_int err = CL_SUCCESS;
  cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
  cl_command_queue queue = clCreateCommandQueue(ctx, device, 0, &err);

  FILE* kf = fopen("../src/kernels/k1_depacketizer.cl", "r");
  check(kf != nullptr, "k1_depacketizer.cl opened");
  std::vector<char> src_buf;
  if (kf) {
    fseek(kf, 0, SEEK_END);
    long sz = ftell(kf);
    fseek(kf, 0, SEEK_SET);
    src_buf.resize(sz + 1, 0);
    size_t nread = fread(src_buf.data(), 1, sz, kf);
    (void)nread;
    fclose(kf);
  }
  const char* src_ptr = src_buf.data();
  cl_program prog = clCreateProgramWithSource(ctx, 1, &src_ptr, nullptr, &err);
  err = clBuildProgram(prog, 1, &device,
                        "-cl-std=CL1.2 -I../../p2a-scaffold/src/kernels -I../../p2a-scaffold/src/host -I../src/kernels",
                        nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t log_len = 0;
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_len);
    std::vector<char> log(log_len + 1, 0);
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, log_len, log.data(), nullptr);
    std::fprintf(stderr, "kernel build failed:\n%s\n", log.data());
  }
  check(err == CL_SUCCESS, "k1_depacketize kernel builds on PoCL");
  cl_kernel kernel = clCreateKernel(prog, "k1_depacketize", &err);
  check(err == CL_SUCCESS, "k1_depacketize kernel created");

  auto run_kernel = [&](const std::vector<oi_frame_desc>& descs, const std::vector<uint8_t>& arena,
                        uint32_t slot_id, std::vector<cl_float2>* re_grid_out, uint32_t* bitmap_out) {
    cl_mem arena_buf =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, arena.size(), (void*)arena.data(), &err);
    cl_mem descs_buf = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      descs.size() * sizeof(oi_frame_desc), (void*)descs.data(), &err);
    std::vector<cl_float2> re_grid(14 * kNofSubcarriersMvp, cl_float2{{0.0f, 0.0f}});
    cl_mem re_grid_buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                        re_grid.size() * sizeof(cl_float2), re_grid.data(), &err);
    uint32_t bitmap = 0;
    cl_mem bitmap_buf =
        clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(uint32_t), &bitmap, &err);
    cl_uint nof_descs = static_cast<cl_uint>(descs.size());

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &arena_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &descs_buf);
    clSetKernelArg(kernel, 2, sizeof(cl_uint), &nof_descs);
    clSetKernelArg(kernel, 3, sizeof(cl_uint), &slot_id);
    clSetKernelArg(kernel, 4, sizeof(cl_mem), &re_grid_buf);
    clSetKernelArg(kernel, 5, sizeof(cl_mem), &bitmap_buf);
    size_t global_size = descs.size();
    clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, re_grid_buf, CL_TRUE, 0, re_grid.size() * sizeof(cl_float2), re_grid.data(), 0,
                        nullptr, nullptr);
    clEnqueueReadBuffer(queue, bitmap_buf, CL_TRUE, 0, sizeof(uint32_t), &bitmap, 0, nullptr, nullptr);
    clReleaseMemObject(arena_buf);
    clReleaseMemObject(descs_buf);
    clReleaseMemObject(re_grid_buf);
    clReleaseMemObject(bitmap_buf);
    *re_grid_out = std::move(re_grid);
    *bitmap_out = bitmap;
  };

  // ---- Test 1: single frame, several symbol_id values, structural + numeric consistency ----
  for (uint8_t symbol_id : {0, 2, 7, 11, 13}) {
    std::vector<cbf16_t> iq;
    std::vector<uint8_t> frame = build_real_frame(*uplane_builder, *ecpri_builder, symbol_id, rgen, &iq);

    // Real OCUDU decode chain: eCPRI -> O-RAN, as ground truth.
    span<const uint8_t> after_eth(frame.data() + OI_WIRE_ETH_HEADER_BYTES, frame.size() - OI_WIRE_ETH_HEADER_BYTES);
    ecpri::packet_parameters ecpri_params;
    span<const uint8_t> oran_payload = ecpri_decoder->decode(after_eth, ecpri_params);
    char label[256];
    std::snprintf(label, sizeof(label), "real eCPRI decode succeeds (symbol_id=%u)", symbol_id);
    check(!oran_payload.empty(), label);

    uplane_message_decoder_results results;
    bool ok = uplane_decoder->decode(results, oran_payload);
    std::snprintf(label, sizeof(label), "real O-RAN U-plane decode succeeds (symbol_id=%u)", symbol_id);
    check(ok, label);
    if (!ok || results.sections.empty()) {
      continue;
    }

    // Our own preparse vs the real decoder's parsed fields.
    oi_oran_preparse_state state{};
    oi_frame_desc desc{};
    auto st = oi_oran_preparse_frame(&state, frame.data(), static_cast<uint32_t>(frame.size()), &desc);
    std::snprintf(label, sizeof(label), "oi_oran_preparse_frame OK (symbol_id=%u)", symbol_id);
    check(st == OI_PREPARSE_OK, label);
    std::snprintf(label, sizeof(label), "preparse symbol_id matches real decoder (symbol_id=%u)", symbol_id);
    check(desc.symbol_id == results.params.symbol_id, label);
    std::snprintf(label, sizeof(label), "preparse section_id matches real decoder (symbol_id=%u)", symbol_id);
    check(desc.section_id == results.sections[0].section_id, label);
    std::snprintf(label, sizeof(label), "preparse start_prb matches real decoder (symbol_id=%u)", symbol_id);
    check(desc.start_prb == results.sections[0].start_prb, label);
    std::snprintf(label, sizeof(label), "preparse nof_prbs matches real decoder (symbol_id=%u)", symbol_id);
    check(desc.nof_prbs == results.sections[0].nof_prbs, label);

    desc.arena_offset = 0;
    desc.frame_len = static_cast<uint32_t>(frame.size());

    // Real kernel via PoCL vs the real decoder's decompressed IQ samples.
    std::vector<oi_frame_desc> descs = {desc};
    std::vector<cl_float2> re_grid;
    uint32_t bitmap = 0;
    run_kernel(descs, frame, desc.slot_id, &re_grid, &bitmap);

    std::snprintf(label, sizeof(label), "K1 kernel sets symbol_bitmap bit (symbol_id=%u)", symbol_id);
    check((bitmap & (1u << symbol_id)) != 0, label);

    // Comparison basis (LLD HLD D5): K1 deliberately keeps fp32 internally ("never OCUDU's
    // cbf16_t") for precision, while OCUDU's own decompress() stores its result as cbf16_t
    // (bf16-rounded) -- so K1's raw float will NOT equal the decoder's stored value bit-for-bit
    // in general (bf16 has only 7 mantissa bits). The correct, strongest test is: does K1's fp32
    // value round-trip to the SAME bf16 value the real decoder stored? That is bit-exact
    // agreement on everything the decoder's own output format is capable of expressing, with
    // zero slop beyond a difference already inherent to (and accepted by) OCUDU's own API.
    bool re_match = true;
    size_t first_mismatch = 0;
    for (size_t sc = 0; sc < kNofSubcarriersMvp; sc++) {
      cbf16_t expected = results.sections[0].iq_samples[sc];
      cl_float2 got = re_grid[symbol_id * kNofSubcarriersMvp + sc];
      if (to_bf16(got.s[0]) != expected.real || to_bf16(got.s[1]) != expected.imag) {
        re_match = false;
        first_mismatch = sc;
        break;
      }
    }
    std::snprintf(label, sizeof(label),
                 "K1 kernel RE grid bit-exact (mod bf16 storage) vs real OCUDU decoder (symbol_id=%u)", symbol_id);
    check(re_match, label);
    if (!re_match) {
      cf_t expected = to_cf(results.sections[0].iq_samples[first_mismatch]);
      cl_float2 got = re_grid[symbol_id * kNofSubcarriersMvp + first_mismatch];
      std::fprintf(stderr, "  first mismatch at subcarrier %zu: expected=(%f,%f) got=(%f,%f)\n", first_mismatch,
                   expected.real(), expected.imag(), got.s[0], got.s[1]);
    }
  }

  // ---- Test 2: truncated frame -> dropped, not fatal (no scatter, bitmap bit stays unset) ----
  {
    std::vector<cbf16_t> iq;
    std::vector<uint8_t> frame = build_real_frame(*uplane_builder, *ecpri_builder, /*symbol_id=*/5, rgen, &iq);
    oi_oran_preparse_state state{};
    oi_frame_desc desc{};
    oi_oran_preparse_frame(&state, frame.data(), static_cast<uint32_t>(frame.size()), &desc);
    desc.arena_offset = 0;
    desc.frame_len = static_cast<uint32_t>(frame.size()) / 2;  // truncate: declared nof_prbs no longer fits

    std::vector<oi_frame_desc> descs = {desc};
    std::vector<uint8_t> truncated_arena(frame.begin(), frame.begin() + desc.frame_len);
    std::vector<cl_float2> re_grid;
    uint32_t bitmap = 0;
    run_kernel(descs, truncated_arena, desc.slot_id, &re_grid, &bitmap);
    check(bitmap == 0, "truncated frame: K1 drops it, symbol_bitmap bit stays unset");
  }

  // ---- Test 3: duplicate frame -> idempotent (bit set once, RE value is one of the two writes,
  // never a sum) ----
  {
    std::vector<cbf16_t> iq;
    std::vector<uint8_t> frame = build_real_frame(*uplane_builder, *ecpri_builder, /*symbol_id=*/4, rgen, &iq);
    oi_oran_preparse_state state{};
    oi_frame_desc desc{};
    oi_oran_preparse_frame(&state, frame.data(), static_cast<uint32_t>(frame.size()), &desc);
    desc.arena_offset = 0;
    desc.frame_len = static_cast<uint32_t>(frame.size());

    std::vector<oi_frame_desc> descs = {desc, desc};  // same descriptor twice: duplicate frame
    std::vector<cl_float2> re_grid;
    uint32_t bitmap = 0;
    run_kernel(descs, frame, desc.slot_id, &re_grid, &bitmap);
    check(bitmap == (1u << 4), "duplicate frame: symbol_bitmap bit set exactly once (no double-count)");

    // Ground truth via the real decoder (not the pre-quantization `iq` -- compress() rounds).
    span<const uint8_t> after_eth(frame.data() + OI_WIRE_ETH_HEADER_BYTES, frame.size() - OI_WIRE_ETH_HEADER_BYTES);
    ecpri::packet_parameters ecpri_params;
    span<const uint8_t> oran_payload = ecpri_decoder->decode(after_eth, ecpri_params);
    uplane_message_decoder_results results;
    uplane_decoder->decode(results, oran_payload);
    cbf16_t decoded0 = results.sections[0].iq_samples[0];
    cl_float2 got = re_grid[4 * kNofSubcarriersMvp + 0];
    (void)iq;
    check(to_bf16(got.s[0]) == decoded0.real && to_bf16(got.s[1]) == decoded0.imag,
          "duplicate frame: RE value is the single real value, not a sum of two writes");
  }

  // ---- Test 4: reordered frames -> scatter is order-independent (symbol-indexed, not
  // stream-ordered) ----
  {
    std::vector<cbf16_t> iq_a, iq_b;
    std::vector<uint8_t> frame_hi = build_real_frame(*uplane_builder, *ecpri_builder, /*symbol_id=*/9, rgen, &iq_a);
    std::vector<uint8_t> frame_lo = build_real_frame(*uplane_builder, *ecpri_builder, /*symbol_id=*/3, rgen, &iq_b);

    oi_oran_preparse_state state{};
    oi_frame_desc desc_hi{}, desc_lo{};
    oi_oran_preparse_frame(&state, frame_hi.data(), static_cast<uint32_t>(frame_hi.size()), &desc_hi);
    oi_oran_preparse_frame(&state, frame_lo.data(), static_cast<uint32_t>(frame_lo.size()), &desc_lo);
    // Concatenate into one arena: frame_hi (symbol 9) placed BEFORE frame_lo (symbol 3), i.e.
    // "arrived" out of symbol order, mirroring the error table's reordering scenario.
    std::vector<uint8_t> arena = frame_hi;
    desc_hi.arena_offset = 0;
    desc_hi.frame_len = static_cast<uint32_t>(frame_hi.size());
    desc_lo.arena_offset = static_cast<uint32_t>(arena.size());
    arena.insert(arena.end(), frame_lo.begin(), frame_lo.end());
    desc_lo.frame_len = static_cast<uint32_t>(frame_lo.size());
    desc_hi.slot_id = 0;
    desc_lo.slot_id = 0;  // preparse gave both slot_id 0 (first frames each stream saw)

    std::vector<oi_frame_desc> descs = {desc_hi, desc_lo};  // array order: symbol 9 THEN symbol 3
    std::vector<cl_float2> re_grid;
    uint32_t bitmap = 0;
    run_kernel(descs, arena, 0, &re_grid, &bitmap);
    check(bitmap == ((1u << 9) | (1u << 3)), "reordered frames: both symbol bits set regardless of array order");

    // Ground truth via the real decoder (NOT the pre-quantization iq_a/iq_b -- compress() rounds,
    // so comparing against the un-quantized input would be a false-mismatch risk, same reasoning
    // as test 3's duplicate-frame check).
    auto decode_re0 = [&](const std::vector<uint8_t>& f) {
      span<const uint8_t> after_eth(f.data() + OI_WIRE_ETH_HEADER_BYTES, f.size() - OI_WIRE_ETH_HEADER_BYTES);
      ecpri::packet_parameters ep;
      span<const uint8_t> payload = ecpri_decoder->decode(after_eth, ep);
      uplane_message_decoder_results r;
      uplane_decoder->decode(r, payload);
      return r.sections[0].iq_samples[0];
    };
    cbf16_t expected_hi = decode_re0(frame_hi);
    cbf16_t expected_lo = decode_re0(frame_lo);
    cl_float2 got_hi = re_grid[9 * kNofSubcarriersMvp + 0];
    cl_float2 got_lo = re_grid[3 * kNofSubcarriersMvp + 0];
    check(to_bf16(got_hi.s[0]) == expected_hi.real && to_bf16(got_lo.s[0]) == expected_lo.real,
          "reordered frames: each symbol's data lands at its own symbol-indexed row, not swapped");
  }

  // ---- Test 5 (2026-07-24): VLAN-tagged frame round-trip -- proves K1 correctly consumes
  // desc.eth_hdr_len (set by oi_oran_preparse_frame's real VLAN detection) rather than assuming a
  // fixed 14-byte Ethernet header. Same structural + bit-exact comparison as Test 1, just with a
  // real 802.1Q tag inserted before the O-RAN payload -- triggered by p1's ru_emulator finding
  // that --vlan_tag has no untagged option (real wire is expected to always carry a tag). ----
  {
    std::vector<cbf16_t> iq;
    uint8_t symbol_id = 6;
    std::vector<uint8_t> frame =
        build_real_frame(*uplane_builder, *ecpri_builder, symbol_id, rgen, &iq, /*vlan_tagged=*/true);

    uint32_t eth_len = OI_WIRE_ETH_HEADER_BYTES_TAGGED;
    span<const uint8_t> after_eth(frame.data() + eth_len, frame.size() - eth_len);
    ecpri::packet_parameters ecpri_params;
    span<const uint8_t> oran_payload = ecpri_decoder->decode(after_eth, ecpri_params);
    check(!oran_payload.empty(), "VLAN-tagged frame: real eCPRI decode succeeds");

    uplane_message_decoder_results results;
    bool ok = uplane_decoder->decode(results, oran_payload);
    check(ok, "VLAN-tagged frame: real O-RAN U-plane decode succeeds");
    if (ok && !results.sections.empty()) {
      oi_oran_preparse_state state{};
      oi_frame_desc desc{};
      auto st = oi_oran_preparse_frame(&state, frame.data(), static_cast<uint32_t>(frame.size()), &desc);
      check(st == OI_PREPARSE_OK, "VLAN-tagged frame: oi_oran_preparse_frame OK");
      check(desc.eth_hdr_len == OI_WIRE_ETH_HEADER_BYTES_TAGGED,
            "VLAN-tagged frame: preparse sets eth_hdr_len == 18");
      check(desc.symbol_id == results.params.symbol_id,
            "VLAN-tagged frame: preparse symbol_id matches real decoder");

      desc.arena_offset = 0;
      desc.frame_len = static_cast<uint32_t>(frame.size());

      std::vector<oi_frame_desc> descs = {desc};
      std::vector<cl_float2> re_grid;
      uint32_t bitmap = 0;
      run_kernel(descs, frame, desc.slot_id, &re_grid, &bitmap);
      check((bitmap & (1u << symbol_id)) != 0, "VLAN-tagged frame: K1 kernel sets symbol_bitmap bit");

      bool re_match = true;
      for (size_t sc = 0; sc < kNofSubcarriersMvp; sc++) {
        cbf16_t expected = results.sections[0].iq_samples[sc];
        cl_float2 got = re_grid[symbol_id * kNofSubcarriersMvp + sc];
        if (to_bf16(got.s[0]) != expected.real || to_bf16(got.s[1]) != expected.imag) {
          re_match = false;
          break;
        }
      }
      check(re_match, "VLAN-tagged frame: K1 kernel RE grid bit-exact (mod bf16) vs real OCUDU decoder "
                     "-- proves K1 correctly used desc.eth_hdr_len==18, not a hardcoded 14");
    }
  }

  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);

  std::printf("\n%s\n", g_fail == 0 ? "k1_test: ALL PASS" : "k1_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
