#pragma once

// Migration SQL, embedded as raw strings and applied in order by
// Database::migrate(). Each entry bumps schema_version by one. Embedded
// (rather than external .sql files) so the binary never depends on a
// runtime-resolvable resource path for something this small — see
// docs/architecture.md.
//
// The db is always disposable and rebuildable from the vault (documents/
// tags/documents_fts) or is itself the source of truth for auth state
// (users/sessions) — either way, nothing here is meant to be hand-edited.

namespace wikicore::index::schema {

// Migration 1: full MVP schema, including the Phase 2 document_snapshots
// table (created but unused — versioning is deliberately out of scope for
// the MVP). document_embeddings (sqlite-vec) is deliberately NOT created
// here either, unlike every other table: it's a vec0 virtual table whose
// column width is fixed at CREATE time to a specific embedding
// provider's dimensionality (384 for the local bge-small model, 1536 for
// OpenAI's cloud one) — baking one into this always-applied migration
// would make the OTHER provider kind permanently unusable without a
// manual DROP. index::EmbeddingIndexer::ensureTable() creates (or
// recreates, on a provider/dimension change) this table on demand
// instead, once an EmbeddingProvider is actually configured and its
// dimensions() are known — see docs/embeddings.md.
inline constexpr const char* kMigration1 = R"sql(
CREATE TABLE documents (
  rowid_id     INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid         TEXT NOT NULL UNIQUE,
  path         TEXT NOT NULL UNIQUE,
  title        TEXT NOT NULL,
  doc_type     TEXT,
  visibility   TEXT NOT NULL CHECK(visibility IN ('public','private')) DEFAULT 'private',
  created_at   TEXT NOT NULL,
  updated_at   TEXT NOT NULL,
  file_mtime   INTEGER NOT NULL,
  file_size    INTEGER NOT NULL,
  content_hash TEXT,
  excerpt      TEXT
);

CREATE TABLE tags (
  id   INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE
);

CREATE TABLE document_tags (
  document_rowid INTEGER NOT NULL REFERENCES documents(rowid_id) ON DELETE CASCADE,
  tag_id         INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (document_rowid, tag_id)
);

CREATE VIRTUAL TABLE documents_fts USING fts5(
  title, body, tags_flat, tokenize = 'porter unicode61'
);

CREATE TABLE attachments (
  id             INTEGER PRIMARY KEY,
  document_rowid INTEGER NOT NULL REFERENCES documents(rowid_id) ON DELETE CASCADE,
  path           TEXT NOT NULL UNIQUE,
  mime_type      TEXT,
  file_size      INTEGER,
  file_mtime     INTEGER
);

CREATE TABLE users (
  id            INTEGER PRIMARY KEY CHECK (id = 1),
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  created_at    TEXT NOT NULL
);

CREATE TABLE sessions (
  token_hash   TEXT PRIMARY KEY,
  user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at   TEXT NOT NULL,
  expires_at   TEXT NOT NULL,
  last_seen_at TEXT NOT NULL,
  csrf_token   TEXT NOT NULL,
  user_agent   TEXT,
  ip           TEXT
);

CREATE TABLE index_meta (
  key   TEXT PRIMARY KEY,
  value TEXT
);

-- Phase 2 — table exists from the start so a future migration only needs
-- to start *using* it, not create it; unused in the MVP.
CREATE TABLE document_snapshots (
  id              INTEGER PRIMARY KEY,
  document_rowid  INTEGER NOT NULL REFERENCES documents(rowid_id) ON DELETE CASCADE,
  snapshot_at     TEXT NOT NULL,
  content         TEXT NOT NULL,
  author          TEXT
);
)sql";

// Migration 2: [[wiki-link]] backlinks (see src/util/WikiLinks.h).
// target_path is plain TEXT, not a FK to documents(path) -- a link to a
// document that doesn't exist yet ("red link") is still worth recording,
// so IndexUpdater::upsertOne can store it as-is: the moment a real
// document lands at that path, NavQueries::backlinks finds the link
// immediately, with no re-save of the document that authored the link.
// ON DELETE CASCADE is only on the SOURCE side (source_rowid) -- deleting
// the *target* document intentionally leaves the linking document's own
// row alone (it just becomes a red link again, same as if the target had
// never existed).
inline constexpr const char* kMigration2 = R"sql(
CREATE TABLE document_links (
  source_rowid INTEGER NOT NULL REFERENCES documents(rowid_id) ON DELETE CASCADE,
  target_path  TEXT NOT NULL,
  PRIMARY KEY (source_rowid, target_path)
);

CREATE INDEX idx_document_links_target ON document_links(target_path);
)sql";

// Migration 3: audit trail for MCP write tools (create_document/
// update_document — see McpServer.cpp, gated behind [mcp].write_access,
// default off). Every call through either tool is recorded here
// regardless of outcome — `success = 0` rows (a rejected path, a
// validation failure, ...) are kept, not discarded, so the admin
// reviewing this table sees attempted writes too, not just ones that
// landed. This table exists independent of write_access's own value:
// turning write_access off after some writes already happened doesn't
// erase the history of what was written while it was on.
inline constexpr const char* kMigration3 = R"sql(
CREATE TABLE mcp_audit_log (
  id        INTEGER PRIMARY KEY,
  at        TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  path      TEXT NOT NULL,
  success   INTEGER NOT NULL,
  detail    TEXT
);

CREATE INDEX idx_mcp_audit_log_at ON mcp_audit_log(at DESC);
)sql";

// Migration 4: remote (HTTP) MCP transport settings — see
// src/auth/McpRemoteConfig.h. Runtime-mutable via the Web UI on purpose
// (not config.toml), so an admin can flip enabled/write_enabled or edit
// the IP allowlist without a server restart. Singleton row (id=1, same
// `CHECK (id = 1)` pattern as `users`) — there is exactly one remote MCP
// configuration for this instance, same as there's exactly one admin
// account. token_hash is nullable: no token has ever been issued until
// the admin's first "regenerate" click.
inline constexpr const char* kMigration4 = R"sql(
CREATE TABLE mcp_remote_config (
  id            INTEGER PRIMARY KEY CHECK (id = 1),
  enabled       INTEGER NOT NULL DEFAULT 0,
  write_enabled INTEGER NOT NULL DEFAULT 0,
  token_hash    TEXT
);

CREATE TABLE mcp_remote_allowed_cidrs (
  id   INTEGER PRIMARY KEY,
  cidr TEXT NOT NULL UNIQUE
);
)sql";

// Migration 5: embeddings skip-if-unchanged tracking + a runtime on/off
// switch for vector search — see docs/embeddings.md's "Skip-if-unchanged
// and self-healing" section.
//
// document_embedding_state tracks, per document, the content hash as of
// the LAST SUCCESSFUL embed — EmbeddingIndexer::EmbeddingIndexer (via
// IndexUpdater::upsertOne) compares a freshly-computed hash against this
// before calling embed() at all, skipping the (potentially slow/network)
// call entirely when nothing has changed. content_hash is '' (never a
// real sha256 hex value) for a document that has NEVER had a successful
// embed — deliberately never NULL, so "no row yet" (LEFT JOIN IS NULL,
// used by the admin-facing "needs attention" query) and "row exists but
// content_hash is unset" stay distinguishable if that's ever needed,
// without relying on NULL doing double duty for both. last_error is
// NULL exactly when the LAST attempt (not necessarily the last SUCCESSFUL
// one) succeeded — a failure leaves content_hash stale (the PRE-failure
// value, or '' if there was never a success) so the mismatch persists and
// the next rescan retries automatically; see IndexUpdater.cpp's own
// comment on why this is real self-healing, not a bug being papered over.
// ON DELETE CASCADE mirrors document_tags/document_snapshots — deleting a
// document must not leave an orphaned tracking row for a rowid that no
// longer exists.
inline constexpr const char* kMigration5 = R"sql(
CREATE TABLE document_embedding_state (
  document_rowid INTEGER PRIMARY KEY REFERENCES documents(rowid_id) ON DELETE CASCADE,
  content_hash    TEXT NOT NULL DEFAULT '',
  last_error      TEXT,
  updated_at      TEXT NOT NULL
);

-- Singleton row, same CHECK(id = 1) pattern as mcp_remote_config — one
-- vector-search on/off switch per instance, flippable from the Web UI
-- with no server restart (see auth/EmbeddingsRuntimeConfig.h). Default
-- ON: an admin who already configured [embeddings] in config.toml
-- expects it active until they explicitly turn it off, not silently
-- disabled by a migration that runs once at first startup after
-- upgrading to this schema version.
CREATE TABLE embeddings_runtime_config (
  id                     INTEGER PRIMARY KEY CHECK (id = 1),
  vector_search_enabled  INTEGER NOT NULL DEFAULT 1
);
)sql";

// Migration 6: persisted Chat panel history (not Draft). Survives
// wiki-server restarts the same way mcp_audit_log does — the in-RAM
// AgentRuntime map is only the live/streaming copy. Salvaged across
// ensureUsable() rebuilds; not vault source-of-truth, not FTS-indexed.
inline constexpr const char* kMigration6 = R"sql(
CREATE TABLE agent_chats (
  id            TEXT PRIMARY KEY,
  title         TEXT NOT NULL,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL,
  events_json   TEXT NOT NULL,
  messages_json TEXT NOT NULL
);
CREATE INDEX idx_agent_chats_updated ON agent_chats(updated_at DESC);
)sql";

}  // namespace wikicore::index::schema
