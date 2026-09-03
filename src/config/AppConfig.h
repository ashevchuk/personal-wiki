#pragma once

#include <cstdint>
#include <string>

namespace wikicore::config {

struct AppConfig {
  // [server]
  std::string listenAddr = "127.0.0.1";
  uint16_t port = 8080;
  size_t threads = 2;

  // [vault]
  std::string vaultPath = "./vault_data";

  // [index]
  std::string dbPath = "./vault_data/.index.db";

  // [mcp]
  // "admin" — sees public + private (default; local stdio spawn by owner).
  // "public" — sees only public. Reserved for a future remote transport.
  std::string mcpScope = "admin";

  // [log]
  std::string logLevel = "info";

  // Loads `path` if it exists (TOML, see config.example.toml); returns
  // defaults unchanged if the file is absent. Throws on a file that exists
  // but fails to parse — a broken config should never be silently ignored.
  static AppConfig load(const std::string& path);
};

}  // namespace wikicore::config
