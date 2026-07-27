// patch_schema_regression_test.cpp — P3-R4's schema half: compiles the PATCHED
// ru_emulator_cli11_schema.cpp + ru_emulator_appconfig.h directly (from patches/files/, the exact
// content the patch series adds/modifies -- not a copy) and parses two real CLI11/YAML configs
// through the REAL schema function: (a) no oracle_injection block at all (upstream-shape config)
// -- must parse successfully with oracle_injection.enabled defaulting to false; (b) an explicit
// oracle_injection block -- must parse successfully and populate every field correctly. Proves
// P3-R4's "same config schema accepted" claim structurally; the other half of P3-R4 (byte-for-
// byte same P1-G1/P1-G2 pass/fail outcome with the patched BINARY on a live rig) is deferred
// (DEFERRED_LIVE_GATES.md) since it needs the live rig this host doesn't have.
#include <cstdio>
#include <string>

#include "CLI/CLI11.hpp"
#include "ocudu/support/config_parsers.h"
#include "ru_emulator_appconfig.h"
#include "ru_emulator_cli11_schema.h"

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

// Parses a YAML string through the real patched schema, matching how ru_emulator's own main()
// invokes CLI11 (subapp.config_formatter(create_yaml_config_parser())), mirrored here at the
// top-level app instead since that's all this test needs.
static bool parse_yaml(const std::string& yaml_text, ocudu::ru_emulator_appconfig* out) {
  CLI::App app("ru_emulator schema regression test");
  app.config_formatter(ocudu::create_yaml_config_parser());
  configure_cli11_with_ru_emulator_appconfig_schema(app, *out);
  try {
    std::istringstream ss(yaml_text);
    app.parse_from_stream(ss);
  } catch (const CLI::ParseError&) {
    return false;
  }
  return true;
}

int main() {
  // --- (a) No oracle_injection block at all -- p1-ran-baseline's REAL, verified-working
  // ru_emu.yml shape (`ru_emu: cells: [...]`, NOT the p3 LLD's own approximate `ru_emu: [...]` --
  // see p1's docker/configs/ru_emu.yml header comment, which already documents this exact
  // correction; this test uses the real shape, not the known-wrong one). ---
  {
    std::string yaml = R"(
log:
  level: info
  filename: stdout
ru_emu:
  cells:
    - network_interface: eth0
      ru_mac_addr: "02:6f:69:00:01:01"
      du_mac_addr: "02:6f:69:00:01:02"
      vlan_tag: 1
      bandwidth: 20
      compr_method_ul: none
      compr_bitwidth_ul: 16
      ul_port_id: [0]
      dl_port_id: [0]
      prach_port_id: [4]
)";
    ocudu::ru_emulator_appconfig cfg;
    bool ok = parse_yaml(yaml, &cfg);
    check(ok, "P3-R4: config with NO oracle_injection block parses successfully through the patched schema");
    if (ok) {
      check(!cfg.ru_cfg.empty(), "at least one ru_emu cell parsed");
      if (!cfg.ru_cfg.empty()) {
        check(cfg.ru_cfg[0].oracle_injection.enabled == false,
              "P3-R4: oracle_injection.enabled defaults to false when the block is absent (byte-identical-to-upstream behavior)");
        check(cfg.ru_cfg[0].oracle_injection.files.empty(),
              "oracle_injection.files stays empty when the block is absent");
        check(cfg.ru_cfg[0].network_interface == "eth0", "unrelated existing field (network_interface) still parses correctly");
        check(cfg.ru_cfg[0].vlan_tag == 1, "unrelated existing field (vlan_tag) still parses correctly");
      }
    }
  }

  // --- (b) Explicit oracle_injection block, enabled ---
  {
    std::string yaml = R"(
log:
  level: info
  filename: stdout
ru_emu:
  cells:
    - network_interface: eth0
      ru_mac_addr: "02:6f:69:00:01:01"
      du_mac_addr: "02:6f:69:00:01:02"
      vlan_tag: 1
      bandwidth: 20
      compr_method_ul: none
      compr_bitwidth_ul: 16
      ul_port_id: [0]
      dl_port_id: [0]
      prach_port_id: [4]
      oracle_injection:
        enabled: true
        eaxc_id: 0
        files: ["/oracle/slot_0000.osg", "/oracle/slot_0001.osg"]
        fail_on_format_mismatch: true
)";
    ocudu::ru_emulator_appconfig cfg;
    bool ok = parse_yaml(yaml, &cfg);
    check(ok, "P3-R2: config WITH an explicit oracle_injection block parses successfully");
    if (ok && !cfg.ru_cfg.empty()) {
      const auto& oi = cfg.ru_cfg[0].oracle_injection;
      check(oi.enabled == true, "oracle_injection.enabled parses as true");
      check(oi.eaxc_id == 0, "oracle_injection.eaxc_id parses correctly");
      check(oi.files.size() == 2, "oracle_injection.files list parses with the correct count");
      if (oi.files.size() == 2) {
        check(oi.files[0] == "/oracle/slot_0000.osg", "oracle_injection.files[0] parses correctly (schedule order preserved)");
        check(oi.files[1] == "/oracle/slot_0001.osg", "oracle_injection.files[1] parses correctly (schedule order preserved)");
      }
      check(oi.fail_on_format_mismatch == true, "oracle_injection.fail_on_format_mismatch parses correctly");
    }
  }

  if (g_fail == 0) {
    std::printf("\npatch_schema_regression_test: ALL PASS\n");
  } else {
    std::fprintf(stderr, "\npatch_schema_regression_test: %d FAILURE(S)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
