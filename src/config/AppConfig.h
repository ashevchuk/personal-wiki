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
  // configuration for the common case — there used to be a base_path
  // setting here for exactly that, removed once the frontend became a
  // fully client-rendered SPA shell + JSON API (see docs/architecture.md):
  // shell.html's own inline bootstrap script infers the mount prefix from
  // location.pathname at load time for every KNOWN route (/d/..., /search,
  // ...), correct automatically with nothing server-side needing to know
  // or care.
  //
  // Brought back here, OPTIONAL, empty by default, after a real deployment
  // hit the one case that client-side pattern-matching cannot ever close:
  // a request whose path matches NO known route (a typo, a stale
  // [[wiki-link]], someone's old bookmark) served by main.cpp's
  // setDefaultHandler, on a browser with nothing yet cached in
  // localStorage — no page load has happened yet to record a known-good
  // prefix, so there is no signal left ANYWHERE client-side to recover
  // it from; the earlier "cache the last known-good prefix" fallback
  // degrades gracefully but stays visibly broken on that first hit.
  // Setting this closes that gap completely: PageRoutes.cpp bakes it into
  // EVERY served shell.html as an authoritative
  // `window.__WIKI_KNOWN_BASE_PATH__`, which the bootstrap script uses
  // instead of guessing, matched route or not. Leave unset for a
  // deployment on its own (sub)domain — pattern-matching already covers
  // that case perfectly, nothing to gain by setting it.
  std::string basePath;

  // Optional, empty by default (meaning "no server-side opinion — the
  // client picks its own hardcoded fallback, currently green"). Set to
  // "classic", "dark", or "green" to make a FRESH browser (no
  // wiki.theme in localStorage yet) land on that theme instead. Same
  // injection mechanism as basePath just above: PageRoutes.cpp bakes
  // this into every served shell.html as
  // `window.__WIKI_DEFAULT_THEME__`, which the bootstrap script's own
  // fallback chain checks before its own hardcoded "green" — a reader
  // who's already picked a theme via the sidebar picker always keeps
  // that choice regardless of this setting; it only affects the very
  // first, cold visit. Deliberately NOT validated here (same as
  // mcpScope just below — a string passed through as-is, no allowlist
  // at this layer): the bootstrap script already checks any value
  // against its own THEMES array before using it, so a typo'd or
  // stale theme name here just falls through to "green" client-side,
  // exactly as if this had been left unset. No reason to duplicate
  // that allowlist on the C++ side too.
  std::string defaultTheme;

  // [vault]
  std::string vaultPath = "./vault_data";

  // [index]
  std::string dbPath = "./vault_data/.index.db";

  // [mcp]
  // "admin" — sees public + private (default; local stdio spawn by owner).
  // "public" — sees only public. Reserved for a future remote transport.
  std::string mcpScope = "admin";
  // Phase 2: create_document/update_document tools. Defaults OFF — MCP
  // clients (an LLM) writing to the vault unsupervised is a materially
  // different risk than read-only search/browse, worth an explicit,
  // conscious opt-in rather than showing up silently the first time
  // this binary gets rebuilt. Every write through these tools (success
  // OR failure) is recorded in the mcp_audit_log table regardless —
  // see McpServer.cpp — so turning this on is "let the LLM write,
  // reviewably", not "let it write invisibly".
  bool mcpWriteAccess = false;

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
