/* oi_p2_config.h — YAML load + MVP-config validator (P2-R11).
 *
 * Parses the single YAML schema (p2-phy-kernels LLD §5, "oi-p2-config/1") and validates every
 * field against its single accepted MVP value. Any deviation is a hard rejection — no partial or
 * best-effort support (P2-R11: "any other config in the YAML is rejected at setup with a
 * structured error, no silent fallback, no partial support").
 */
#ifndef OI_P2_CONFIG_H
#define OI_P2_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace oi_p2 {

/// The exact set of scalar values this pipeline accepts (SPEC "Fixed MVP configuration").
/// Populated by oi_p2_config_load on success; never partially populated on failure.
struct MvpConfig {
  std::string duplex;              // "tdd"
  std::string band;                // "n78"
  int numerology = 0;               // 1
  int bandwidth_mhz = 0;             // 20
  int nof_ues = 0;                   // 1
  int nof_layers = 0;                // 1
  int nof_rx_ports = 0;              // 1
  int rb_start = 0;                  // 0
  int nof_prb = 0;                   // 51
  int symbol_start = 0;              // 0
  int nof_symbols = 0;               // 14
  std::string mapping_type;         // "A"
  int dmrs_type = 0;                 // 1
  int dmrs_additional_positions = 0; // 2
  int dmrs_cdm_groups_without_data = 0; // 2
  std::vector<int> mcs_set;         // {4, 13, 21}, any order
  bool harq_new_data_always = false; // true
  int harq_rv = -1;                  // 0
  std::string uci_on_pusch;         // "none"
  int scrambling_n_id = -1;          // 1
  uint32_t scrambling_rnti = 0;      // 0x4601
  std::vector<int> ofh_eaxc;        // {0}
  std::string ofh_compression;      // "uncompressed_16bit"
  bool ofh_vlan = true;              // false
  std::string oracle_srsran_vectors_path;   // optional
  std::string oracle_sionna_vectors_path;   // required, must exist on disk
  std::string device_platform_env;  // "OI_CL_PLATFORM"
};

enum class ConfigStatus {
  OK = 0,
  ERR_PARSE = 1,          // file doesn't exist or isn't valid YAML
  ERR_SCHEMA_MISMATCH = 2, // schema tag != "oi-p2-config/1"
  ERR_FIELD_REJECTED = 3,  // a field is present but not the single accepted MVP value
  ERR_MISSING_REQUIRED = 4, // a required field/path is absent (e.g. sionna_vectors_path)
};

/// Structured rejection detail: which key, what was read, what was expected. Populated whenever
/// oi_p2_config_load returns anything other than ConfigStatus::OK.
struct ConfigError {
  std::string key;
  std::string got;
  std::string expected;
};

/// Loads and validates yaml_path against the fixed MVP shape. On ConfigStatus::OK, out_config is
/// fully populated; on any other status, out_error names the first offending key (LLD §5:
/// "First violation short-circuits") and out_config is left default-constructed (never partial).
ConfigStatus oi_p2_config_load(const std::string& yaml_path, MvpConfig* out_config,
                                ConfigError* out_error);

}  // namespace oi_p2

#endif /* OI_P2_CONFIG_H */
