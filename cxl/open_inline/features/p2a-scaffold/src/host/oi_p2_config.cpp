#include "oi_p2_config.h"

#include <sys/stat.h>

#include <algorithm>
#include <yaml-cpp/yaml.h>

namespace oi_p2 {

namespace {

bool path_exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

std::string to_string_any(const YAML::Node& n) {
  if (!n) return "<absent>";
  try {
    return n.as<std::string>();
  } catch (...) {
    return "<unparseable>";
  }
}

// Sets *err and returns false if node is absent or its scalar value != expected.
template <typename T>
bool require_scalar(const YAML::Node& parent, const char* key, const T& expected,
                     const std::string& key_path, ConfigError* out_error) {
  YAML::Node n = parent[key];
  if (!n) {
    *out_error = {key_path, "<absent>", to_string_any(YAML::Node(expected))};
    return false;
  }
  T got;
  try {
    got = n.as<T>();
  } catch (...) {
    *out_error = {key_path, to_string_any(n), to_string_any(YAML::Node(expected))};
    return false;
  }
  if (!(got == expected)) {
    *out_error = {key_path, to_string_any(n), to_string_any(YAML::Node(expected))};
    return false;
  }
  return true;
}

}  // namespace

ConfigStatus oi_p2_config_load(const std::string& yaml_path, MvpConfig* out_config,
                                ConfigError* out_error) {
  *out_config = MvpConfig{};
  *out_error = ConfigError{};

  YAML::Node root;
  try {
    root = YAML::LoadFile(yaml_path);
  } catch (const std::exception& e) {
    *out_error = {"<file>", yaml_path, "a valid YAML file"};
    return ConfigStatus::ERR_PARSE;
  }

  // 1. Schema tag match.
  if (!require_scalar<std::string>(root, "schema", "oi-p2-config/1", "schema", out_error)) {
    return ConfigStatus::ERR_SCHEMA_MISMATCH;
  }

  // 2. Every scalar field equals its single accepted MVP value (SPEC "Fixed MVP configuration").
#define REQUIRE(parent, key, expected, path)                                                     \
  if (!require_scalar(parent, key, expected, path, out_error)) return ConfigStatus::ERR_FIELD_REJECTED;

  REQUIRE(root, "duplex", std::string("tdd"), "duplex");
  REQUIRE(root, "band", std::string("n78"), "band");
  REQUIRE(root, "numerology", 1, "numerology");
  REQUIRE(root, "bandwidth_mhz", 20, "bandwidth_mhz");
  REQUIRE(root, "nof_ues", 1, "nof_ues");
  REQUIRE(root, "nof_layers", 1, "nof_layers");
  REQUIRE(root, "nof_rx_ports", 1, "nof_rx_ports");

  YAML::Node pusch = root["pusch_allocation"];
  if (!pusch) {
    *out_error = {"pusch_allocation", "<absent>", "present"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  REQUIRE(pusch, "rb_start", 0, "pusch_allocation.rb_start");
  REQUIRE(pusch, "nof_prb", 51, "pusch_allocation.nof_prb");
  REQUIRE(pusch, "symbol_start", 0, "pusch_allocation.symbol_start");
  REQUIRE(pusch, "nof_symbols", 14, "pusch_allocation.nof_symbols");
  REQUIRE(pusch, "mapping_type", std::string("A"), "pusch_allocation.mapping_type");

  YAML::Node dmrs = root["dmrs"];
  if (!dmrs) {
    *out_error = {"dmrs", "<absent>", "present"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  REQUIRE(dmrs, "type", 1, "dmrs.type");
  REQUIRE(dmrs, "additional_positions", 2, "dmrs.additional_positions");
  REQUIRE(dmrs, "cdm_groups_without_data", 2, "dmrs.cdm_groups_without_data");

  // 3. mcs_set is exactly {4, 13, 21} in any order.
  YAML::Node mcs_node = root["mcs_set"];
  if (!mcs_node || !mcs_node.IsSequence()) {
    *out_error = {"mcs_set", "<absent-or-not-a-list>", "[4, 13, 21] (any order)"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  std::vector<int> mcs_set;
  for (const auto& v : mcs_node) mcs_set.push_back(v.as<int>());
  std::vector<int> mcs_sorted = mcs_set;
  std::sort(mcs_sorted.begin(), mcs_sorted.end());
  if (mcs_sorted != std::vector<int>{4, 13, 21}) {
    std::string got_str;
    for (int v : mcs_set) got_str += std::to_string(v) + ",";
    *out_error = {"mcs_set", got_str, "4,13,21 (any order, no other values)"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  out_config->mcs_set = mcs_set;

  YAML::Node harq = root["harq"];
  if (!harq) {
    *out_error = {"harq", "<absent>", "present"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  REQUIRE(harq, "new_data_always", true, "harq.new_data_always");
  REQUIRE(harq, "rv", 0, "harq.rv");

  REQUIRE(root, "uci_on_pusch", std::string("none"), "uci_on_pusch");

  YAML::Node scr = root["scrambling"];
  if (!scr) {
    *out_error = {"scrambling", "<absent>", "present"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  REQUIRE(scr, "n_id", 1, "scrambling.n_id");
  // rnti: YAML may parse 0x4601 as int already (yaml-cpp handles 0x-prefixed ints).
  if (!scr["rnti"] || scr["rnti"].as<uint32_t>() != 0x4601u) {
    *out_error = {"scrambling.rnti", to_string_any(scr["rnti"]), "0x4601"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  out_config->scrambling_rnti = 0x4601;

  YAML::Node ofh = root["ofh"];
  if (!ofh) {
    *out_error = {"ofh", "<absent>", "present"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  YAML::Node eaxc = ofh["eaxc"];
  if (!eaxc || !eaxc.IsSequence() || eaxc.size() != 1 || eaxc[0].as<int>() != 0) {
    *out_error = {"ofh.eaxc", to_string_any(eaxc), "[0]"};
    return ConfigStatus::ERR_FIELD_REJECTED;
  }
  REQUIRE(ofh, "compression", std::string("uncompressed_16bit"), "ofh.compression");
  REQUIRE(ofh, "vlan", false, "ofh.vlan");

#undef REQUIRE

  // 4. Oracle paths: sionna required+must-exist; srsran optional.
  YAML::Node oracle = root["oracle"];
  if (!oracle || !oracle["sionna_vectors_path"]) {
    *out_error = {"oracle.sionna_vectors_path", "<absent>", "a path that exists on disk"};
    return ConfigStatus::ERR_MISSING_REQUIRED;
  }
  std::string sionna_path = oracle["sionna_vectors_path"].as<std::string>();
  if (!path_exists(sionna_path)) {
    *out_error = {"oracle.sionna_vectors_path", sionna_path, "a path that exists on disk"};
    return ConfigStatus::ERR_MISSING_REQUIRED;
  }
  out_config->oracle_sionna_vectors_path = sionna_path;

  if (oracle["srsran_vectors_path"]) {
    out_config->oracle_srsran_vectors_path = oracle["srsran_vectors_path"].as<std::string>();
    // Absence is fine (P2-R14a downgrade to skipped/logged, never a hard failure here) — this
    // validator only records the path if present; whether it exists on disk is checked by the
    // oracle harness at run time, not by config validation (that oracle is CI-only per P2-R13,
    // and CI-runner availability is not this validator's concern).
  }

  YAML::Node device = root["device"];
  if (device && device["platform_env"]) {
    out_config->device_platform_env = device["platform_env"].as<std::string>();
  } else {
    out_config->device_platform_env = "OI_CL_PLATFORM";
  }

  // All checks passed: populate the remaining plain fields.
  out_config->duplex = "tdd";
  out_config->band = "n78";
  out_config->numerology = 1;
  out_config->bandwidth_mhz = 20;
  out_config->nof_ues = 1;
  out_config->nof_layers = 1;
  out_config->nof_rx_ports = 1;
  out_config->rb_start = 0;
  out_config->nof_prb = 51;
  out_config->symbol_start = 0;
  out_config->nof_symbols = 14;
  out_config->mapping_type = "A";
  out_config->dmrs_type = 1;
  out_config->dmrs_additional_positions = 2;
  out_config->dmrs_cdm_groups_without_data = 2;
  out_config->harq_new_data_always = true;
  out_config->harq_rv = 0;
  out_config->uci_on_pusch = "none";
  out_config->scrambling_n_id = 1;
  out_config->ofh_eaxc = {0};
  out_config->ofh_compression = "uncompressed_16bit";
  out_config->ofh_vlan = false;

  return ConfigStatus::OK;
}

}  // namespace oi_p2
