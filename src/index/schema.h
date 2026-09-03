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
// the MVP). document_embeddings (sqlite-vec, Phase 2 semantic search) is
// NOT created here: it needs the vec0 virtual table module loaded via
// extension, which isn't wired up yet — adding that table prematurely
// would reference a module that doesn't exist.
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

}  // namespace wikicore::index::schema
