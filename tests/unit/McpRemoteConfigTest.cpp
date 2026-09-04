#include "auth/McpRemoteConfig.h"
#include "index/Database.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::auth;
using wikicore::index::Database;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-mcpremote-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".db")) {
    fs::remove(path_);
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST_CASE("McpRemoteConfig::get defaults to fully off with no token", "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto settings = cfg.get();
  REQUIRE_FALSE(settings.enabled);
  REQUIRE_FALSE(settings.writeEnabled);
  REQUIRE_FALSE(settings.hasToken);
}

TEST_CASE("McpRemoteConfig: setEnabled/setWriteEnabled are independent -- setting "
          "one never resets the other or the token",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const std::string token = cfg.regenerateToken();
  cfg.setEnabled(true);
  cfg.setWriteEnabled(true);

  auto settings = cfg.get();
  REQUIRE(settings.enabled);
  REQUIRE(settings.writeEnabled);
  REQUIRE(settings.hasToken);
  REQUIRE(cfg.verifyToken(token));

  // Flipping enabled off must not touch write_enabled or the token.
  cfg.setEnabled(false);
  settings = cfg.get();
  REQUIRE_FALSE(settings.enabled);
  REQUIRE(settings.writeEnabled);
  REQUIRE(settings.hasToken);
  REQUIRE(cfg.verifyToken(token));
}

TEST_CASE("McpRemoteConfig::regenerateToken invalidates the previous token "
          "immediately",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const std::string first = cfg.regenerateToken();
  REQUIRE(cfg.verifyToken(first));

  const std::string second = cfg.regenerateToken();
  REQUIRE(second != first);
  REQUIRE(cfg.verifyToken(second));
  REQUIRE_FALSE(cfg.verifyToken(first));
}

TEST_CASE("McpRemoteConfig::verifyToken is false before any token was ever "
          "generated, and for garbage input, never throws",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  REQUIRE_FALSE(cfg.verifyToken("anything"));
  REQUIRE_NOTHROW(cfg.verifyToken(""));
  REQUIRE_FALSE(cfg.verifyToken(""));
}

TEST_CASE("McpRemoteConfig::isIpAllowed: an EMPTY allowlist allows everything "
          "(the token is the primary gate, IP-restriction is opt-in)",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  REQUIRE(cfg.isIpAllowed("203.0.113.5"));
  REQUIRE(cfg.isIpAllowed("2001:db8::1"));
}

TEST_CASE("McpRemoteConfig::isIpAllowed: once entries exist, only matching "
          "IPs pass",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  cfg.addAllowedCidr("203.0.113.0/24");
  cfg.addAllowedCidr("2001:db8::1");

  REQUIRE(cfg.isIpAllowed("203.0.113.42"));
  REQUIRE(cfg.isIpAllowed("2001:db8::1"));
  REQUIRE_FALSE(cfg.isIpAllowed("198.51.100.1"));
  REQUIRE_FALSE(cfg.isIpAllowed("2001:db8::2"));
}

TEST_CASE("McpRemoteConfig::removeAllowedCidr removes exactly that entry", "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  cfg.addAllowedCidr("203.0.113.0/24");
  cfg.addAllowedCidr("198.51.100.0/24");
  REQUIRE(cfg.listAllowedCidrs().size() == 2);

  cfg.removeAllowedCidr("203.0.113.0/24");
  const auto remaining = cfg.listAllowedCidrs();
  REQUIRE(remaining.size() == 1);
  REQUIRE(remaining[0] == "198.51.100.0/24");
  // Removing the LAST entry goes back to "no restriction" (empty list),
  // not a mistaken "allow nothing" — same rule as the fresh-install case.
  cfg.removeAllowedCidr("198.51.100.0/24");
  REQUIRE(cfg.listAllowedCidrs().empty());
  REQUIRE(cfg.isIpAllowed("1.2.3.4"));
}

TEST_CASE("McpRemoteConfig::addAllowedCidr is idempotent (a duplicate add "
          "doesn't create a second row)",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  cfg.addAllowedCidr("203.0.113.0/24");
  cfg.addAllowedCidr("203.0.113.0/24");
  REQUIRE(cfg.listAllowedCidrs().size() == 1);
}
