#include "config/AppConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace wikicore::config;

namespace {

// Writes `contents` to a fresh temp file and returns its path — removed on
// destruction so a failed assertion doesn't leak scratch files across runs.
class TempConfigFile {
 public:
  explicit TempConfigFile(const std::string& contents)
      : path_(fs::temp_directory_path() /
              fs::path("wiki-appconfig-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".toml")) {
    std::ofstream(path_) << contents;
  }
  ~TempConfigFile() { fs::remove(path_); }
  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST_CASE("AppConfig::load: base_path defaults to empty when config.toml has no "
          "[server] table at all",
          "[AppConfig]") {
  TempConfigFile file("[vault]\npath = \"./vault_data\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath.empty());
}

TEST_CASE("AppConfig::load: base_path is read verbatim when it has no trailing slash",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: a trailing slash on base_path is stripped — "
          "PageRoutes.cpp always appends its own",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki/\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: repeated trailing slashes are all stripped",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki///\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: a bare \"/\" collapses to empty — same meaning as "
          "\"no prefix\", not a one-character prefix",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath.empty());
}
