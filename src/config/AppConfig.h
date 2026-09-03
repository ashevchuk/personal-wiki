#pragma once

#include <cstdint>
#include <string>

namespace wikicore::config {

struct AppConfig {
  // [server]
  std::string listenAddr = "127.0.0.1";
  uint16_t port = 8080;
  size_t threads = 2;
  // Mount-point prefix for reverse-proxying this app under a subpath
  // (e.g. "/wiki") instead of a whole (sub)domain. Empty by default — the
  // app then behaves exactly as before, every route/link/redirect at
  // domain root. Normalized by util::normalizeBasePath (see AppConfig::load)
  // so callers never need to think about trailing slashes. See
  // docs/deployment.md's reverse-proxy section for the nginx recipe this
  // enables: a PLAIN prefix-stripping proxy_pass, no sub_filter/
  // proxy_redirect body/header rewriting needed on the proxy side, because
  // every href/action/hx-*/redirect this app emits already carries the
  // prefix when it's set.
  std::string basePath;

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
