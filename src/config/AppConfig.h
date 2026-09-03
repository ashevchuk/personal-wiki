#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wikicore::config {

struct AppConfig {
  // [server]
  std::string listenAddr = "127.0.0.1";
  uint16_t port = 8080;
  size_t threads = 2;
  // Reverse-proxying under a subpath (e.g. "/wiki") needs NO server-side
  // configuration — there used to be a base_path setting here for
  // exactly that, but a fully client-rendered frontend (static SPA shell
  // + JSON API, see docs/architecture.md) makes it unnecessary: the
  // shell's own inline bootstrap script infers the mount prefix from
  // location.pathname at load time (see static/shell.html and
  // common.js's basePath()), correct for a plain prefix-stripping nginx
  // `location /wiki/ { proxy_pass ...; }` (or any other subpath)
  // automatically, with nothing server-side needing to know or care.

  // [vault]
  std::string vaultPath = "./vault_data";

  // [index]
  std::string dbPath = "./vault_data/.index.db";

  // [mcp]
  // "admin" — sees public + private (default; local stdio spawn by owner).
  // "public" — sees only public. Reserved for a future remote transport.
  std::string mcpScope = "admin";

  // [attachments] — empty means "use AttachmentService's own built-in
  // defaults" (see main.cpp and AttachmentService::defaultMimeTypes() /
  // defaultInlineSafeExtensions()); config.toml intentionally isn't
  // required to duplicate that whole list just to run. If
  // [attachments.mime_types] / inline_safe_extensions IS present in
  // config.toml, it REPLACES the built-in list entirely (not merged) —
  // see config.example.toml's comment on why, and copy the full default
  // list there if you just want to add one entry.
  std::unordered_map<std::string, std::string> attachmentMimeTypes;
  std::unordered_set<std::string> attachmentInlineSafeExtensions;

  // [log]
  std::string logLevel = "info";

  // Loads `path` if it exists (TOML, see config.example.toml); returns
  // defaults unchanged if the file is absent. Throws on a file that exists
  // but fails to parse — a broken config should never be silently ignored.
  static AppConfig load(const std::string& path);
};

}  // namespace wikicore::config
