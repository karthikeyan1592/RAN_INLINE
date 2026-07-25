// config_test.cpp — P2-R11 test plan: exact MVP YAML accepted; each single-field perturbation
// individually rejected with the specific offending key named.
#include "../src/host/oi_p2_config.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using oi_p2::ConfigError;
using oi_p2::ConfigStatus;
using oi_p2::MvpConfig;
using oi_p2::oi_p2_config_load;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    g_fail++;
  } else {
    std::printf("PASS: %s\n", what.c_str());
  }
}

static std::string read_file(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string write_tmp(const std::string& content, const std::string& name) {
  std::string path = "/tmp/" + name;
  std::ofstream f(path);
  f << content;
  return path;
}

// Applies one textual perturbation to the golden fixture and returns the path to the mutated file.
static std::string perturbed(const std::string& golden, const std::string& from,
                              const std::string& to, const std::string& tag) {
  std::string mutated = golden;
  size_t pos = mutated.find(from);
  if (pos == std::string::npos) {
    std::fprintf(stderr, "test bug: perturbation target '%s' not found in fixture\n", from.c_str());
    std::exit(2);
  }
  mutated.replace(pos, from.size(), to);
  return write_tmp(mutated, "p2a_config_test_" + tag + ".yaml");
}

int main() {
  const std::string fixture_path = "fixtures/mvp_config.yaml";
  const std::string golden = read_file(fixture_path);
  if (golden.empty()) {
    std::fprintf(stderr, "test bug: fixture %s empty/unreadable (run from tests/ dir)\n",
                 fixture_path.c_str());
    return 2;
  }

  // 1. Exact MVP YAML is accepted.
  {
    MvpConfig cfg;
    ConfigError err;
    ConfigStatus st = oi_p2_config_load(fixture_path, &cfg, &err);
    check(st == ConfigStatus::OK, "exact MVP config -> OK");
    check(cfg.numerology == 1 && cfg.nof_prb == 51 && cfg.mcs_set.size() == 3,
          "accepted config populates fields correctly");
  }

  // 2. Each single-field perturbation is individually rejected, naming the offending key.
  struct Case {
    const char* tag;
    const char* from;
    const char* to;
    const char* expect_key_substr;
  };
  const Case cases[] = {
      {"numerology", "numerology: 1", "numerology: 0", "numerology"},
      {"bandwidth", "bandwidth_mhz: 20", "bandwidth_mhz: 15", "bandwidth_mhz"},
      {"nof_layers", "nof_layers: 1", "nof_layers: 2", "nof_layers"},
      {"mcs_bad", "mcs_set: [4, 13, 21]", "mcs_set: [4, 13, 22]", "mcs_set"},
      {"rv_nonzero", "rv: 0", "rv: 1", "harq.rv"},
      {"uci_present", "uci_on_pusch: none", "uci_on_pusch: enabled", "uci_on_pusch"},
      {"compression", "compression: uncompressed_16bit", "compression: bfp9", "ofh.compression"},
      {"schema_bad", "schema: oi-p2-config/1", "schema: oi-p2-config/2", "schema"},
  };

  for (const auto& c : cases) {
    std::string path = perturbed(golden, c.from, c.to, c.tag);
    MvpConfig cfg;
    ConfigError err;
    ConfigStatus st = oi_p2_config_load(path, &cfg, &err);
    check(st != ConfigStatus::OK, std::string("perturbation '") + c.tag + "' is rejected");
    check(err.key.find(c.expect_key_substr) != std::string::npos,
          std::string("perturbation '") + c.tag + "' names key containing '" +
              c.expect_key_substr + "' (got: '" + err.key + "')");
  }

  // 3. Missing required oracle path is rejected distinctly.
  {
    std::string path = perturbed(golden, "sionna_vectors_path: /tmp/fake_sionna_vectors",
                                  "sionna_vectors_path: /nonexistent/path/xyz", "sionna_missing");
    MvpConfig cfg;
    ConfigError err;
    ConfigStatus st = oi_p2_config_load(path, &cfg, &err);
    check(st == ConfigStatus::ERR_MISSING_REQUIRED, "missing sionna path -> ERR_MISSING_REQUIRED");
  }

  // 4. Absent (not just wrong) srsran_vectors_path is NOT a hard failure (P2-R14a downgrade rule).
  {
    std::string mutated = golden;
    size_t pos = mutated.find("  srsran_vectors_path: /root/linux_env/cxl/third_party/srsRAN_Project\n");
    if (pos != std::string::npos) mutated.erase(pos, std::string("  srsran_vectors_path: /root/linux_env/cxl/third_party/srsRAN_Project\n").size());
    std::string path = write_tmp(mutated, "p2a_config_test_no_srsran.yaml");
    MvpConfig cfg;
    ConfigError err;
    ConfigStatus st = oi_p2_config_load(path, &cfg, &err);
    check(st == ConfigStatus::OK, "absent (optional) srsran_vectors_path is still OK");
    check(cfg.oracle_srsran_vectors_path.empty(), "absent srsran path leaves field empty, not an error");
  }

  std::printf("\n%s\n", g_fail == 0 ? "config_test: ALL PASS" : "config_test: FAILURES ABOVE");
  return g_fail == 0 ? 0 : 1;
}
