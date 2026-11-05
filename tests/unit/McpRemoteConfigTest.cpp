#include "auth/McpRemoteConfig.h"
#include "index/Database.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("McpRemoteConfig::get defaults to fully off with no tokens", "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto settings = cfg.get();
  REQUIRE_FALSE(settings.enabled);
  REQUIRE_FALSE(settings.writeEnabled);
  REQUIRE(cfg.listTokens().empty());
}

TEST_CASE("McpRemoteConfig: setEnabled/setWriteEnabled are independent -- setting "
          "one never resets the other or any token",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto token = cfg.createToken("tray-app");
  cfg.setEnabled(true);
  cfg.setWriteEnabled(true);

  auto settings = cfg.get();
  REQUIRE(settings.enabled);
  REQUIRE(settings.writeEnabled);
  REQUIRE(cfg.verifyToken(token.rawToken).matched);

  // Flipping enabled off must not touch write_enabled or any token.
  cfg.setEnabled(false);
  settings = cfg.get();
  REQUIRE_FALSE(settings.enabled);
  REQUIRE(settings.writeEnabled);
  REQUIRE(cfg.verifyToken(token.rawToken).matched);
}

TEST_CASE("McpRemoteConfig::regenerateToken invalidates the previous value of "
          "the SAME token, but leaves other tokens untouched",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto a = cfg.createToken("tray-app");
  const auto b = cfg.createToken("claude-desktop");
  REQUIRE(cfg.verifyToken(a.rawToken).matched);
  REQUIRE(cfg.verifyToken(b.rawToken).matched);

  const auto regenerated = cfg.regenerateToken(a.id);
  REQUIRE(regenerated.id == a.id);
  REQUIRE(regenerated.rawToken != a.rawToken);
  REQUIRE_FALSE(cfg.verifyToken(a.rawToken).matched);
  REQUIRE(cfg.verifyToken(regenerated.rawToken).matched);
  // The other, unrelated token keeps working.
  REQUIRE(cfg.verifyToken(b.rawToken).matched);
}

TEST_CASE("McpRemoteConfig::deleteToken revokes exactly that token, not others",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto a = cfg.createToken("tray-app");
  const auto b = cfg.createToken("claude-desktop");

  cfg.deleteToken(a.id);
  REQUIRE_FALSE(cfg.verifyToken(a.rawToken).matched);
  REQUIRE(cfg.verifyToken(b.rawToken).matched);
  REQUIRE(cfg.listTokens().size() == 1);
}

TEST_CASE("McpRemoteConfig::verifyToken reports the matching token's label and "
          "stamps last_used_at",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto token = cfg.createToken("tray-app");
  REQUIRE(cfg.listTokens()[0].lastUsedAt.empty());

  const auto match = cfg.verifyToken(token.rawToken);
  REQUIRE(match.matched);
  REQUIRE(match.label == "tray-app");
  REQUIRE_FALSE(cfg.listTokens()[0].lastUsedAt.empty());
}

TEST_CASE("McpRemoteConfig::setAgentAccess flips exactly one token's flag, "
          "defaults to off, and is reflected by verifyToken",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  const auto a = cfg.createToken("tray-app");
  const auto b = cfg.createToken("claude-desktop");
  REQUIRE_FALSE(cfg.listTokens()[0].agentAccess);
  REQUIRE_FALSE(cfg.verifyToken(a.rawToken).agentAccess);

  cfg.setAgentAccess(a.id, true);
  const auto tokens = cfg.listTokens();
  const auto findById = [&](int64_t id) {
    return *std::find_if(tokens.begin(), tokens.end(),
                          [&](const auto& t) { return t.id == id; });
  };
  REQUIRE(findById(a.id).agentAccess);
  REQUIRE_FALSE(findById(b.id).agentAccess);
  REQUIRE(cfg.verifyToken(a.rawToken).agentAccess);
  REQUIRE_FALSE(cfg.verifyToken(b.rawToken).agentAccess);
}

TEST_CASE("McpRemoteConfig::verifyToken is false before any token was ever "
          "generated, and for garbage input, never throws",
          "[McpRemoteConfig]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  McpRemoteConfig cfg(database);

  REQUIRE_FALSE(cfg.verifyToken("anything").matched);
  REQUIRE_NOTHROW(cfg.verifyToken(""));
  REQUIRE_FALSE(cfg.verifyToken("").matched);
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
