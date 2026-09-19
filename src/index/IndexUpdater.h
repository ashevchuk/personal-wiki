#pragma once

#include "embeddings/EmbeddingProvider.h"
#include "index/Database.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct DocumentIndexEntry {
  std::string uuid;
  std::string path;  // vault-relative, unique
  std::string title;
  std::string docType;
  std::string visibility;  // "public" | "private"
  std::string createdAt;
  std::string updatedAt;
  int64_t fileMtime = 0;
  int64_t fileSize = 0;
  std::string excerpt;
  std::vector<std::string> tags;
  std::string body;  // fed into documents_fts, not stored as its own column
};

// Keeps the SQLite index in sync with one document at a time, as it's
// saved/deleted through the Web UI (DocumentService calls this after every
// write). This is deliberately NOT the full-vault rescan (that's
// IndexBuilder, Milestone 3) — it only ever touches the one row it's told
// about.
class IndexUpdater {
 public:
  // provider: optional (nullptr = no embedding computed on write — matches
  // embeddings.provider="none", or a build without WIKI_ENABLE_SQLITE_VEC;
  // see docs/embeddings.md). Not owned — must outlive this IndexUpdater
  // (main.cpp constructs and owns both). A failed embed() call (network
  // error for the cloud provider, model issue for local) is caught and
  // swallowed inside upsertOne() rather than failing the whole document
  // save — see that method's own comment for why this is a deliberate,
  // documented best-effort step and not the "silent helper failure" shape
  // that's normally a bug: FTS5/tags (committed just before this runs)
  // remain the authoritative, always-succeeding search path, and a
  // missed embedding self-heals on the next `--reindex` once it's wired
  // up there.
  // minEmbeddingWords: a document whose title+body combined has FEWER
  // words than this never gets a real embed() call at all — see
  // upsertOne()'s own comment for why. Irrelevant when provider is
  // nullptr.
  explicit IndexUpdater(Database& db, embeddings::EmbeddingProvider* provider = nullptr,
                         int minEmbeddingWords = 6)
      : db_(db), provider_(provider), minEmbeddingWords_(minEmbeddingWords) {}

  // Swaps the embedding provider after construction — used by wiki-mcp
  // (mcp/McpServer.cpp) to lazily construct a real provider only on the
  // FIRST actual create_document/update_document call, rather than at
  // process startup: unlike wiki-server, wiki-mcp is spawned fresh per
  // MCP session and is documented project-wide as staying
  // fast-starting/dependency-light (see CLAUDE.md's two-binary-layout
  // entry), so a read-only session (the common case — search_documents/
  // get_document/list_tags/list_documents never touch this) shouldn't
  // pay for loading a local model or validating a cloud API key at all.
  // Not used by wiki-server, which already knows its provider (or lack
  // of one) up front and passes it straight to the constructor.
  void setProvider(embeddings::EmbeddingProvider* provider) { provider_ = provider; }

  // Inserts the row for entry.path if new, or updates it in place if the
  // path is already indexed (path is UNIQUE). Replaces the document's tag
  // set and FTS entry to match `entry` exactly. Returns the row's
  // rowid_id. Runs as a single transaction.
  int64_t upsertOne(const DocumentIndexEntry& entry);

  // Removes the row at `path` (tags/attachments/FTS entry cascade via the
  // schema's ON DELETE CASCADE / explicit FTS cleanup below). No-op if the
  // path isn't indexed.
  void removeOne(const std::string& path);

  // Every currently-indexed document path — used by IndexBuilder's stale
  // sweep to find rows whose file no longer exists on disk.
  std::vector<std::string> allIndexedPaths() const;

  // Resolves a document's uuid to its current vault-relative path — used
  // by the MCP get_document tool's "id_or_path" parameter. nullopt if
  // unknown (including: not indexed yet, e.g. a document created directly
  // on disk before any rescan).
  std::optional<std::string> findPathByUuid(const std::string& uuid) const;

  // The `documents.rowid_id` for `path`, or nullopt if it isn't indexed.
  // Public wrapper around the same lookup upsertOne already does
  // internally — used by DocumentService (to snapshot the pre-edit
  // state against the right row) and VersionRoutes (to resolve a
  // document's history without duplicating this query).
  std::optional<int64_t> rowIdForPath(const std::string& path) const;

 private:
  Database& db_;
  embeddings::EmbeddingProvider* provider_ = nullptr;
  int minEmbeddingWords_ = 6;

  // Guards EVERY BEGIN IMMEDIATE...COMMIT/ROLLBACK sequence this class
  // (and its embedding step) runs against `db_` — documents/tags/FTS in
  // upsertOne/removeOne, AND EmbeddingIndexer::ensureTable/upsertOne/
  // recordEmbeddingSuccess/recordEmbeddingFailure's own transactions.
  // `db_` is a single sqlite3* connection shared by reference across
  // every Drogon request-handling thread (see main.cpp — the SAME
  // reasoning that already gave VaultWatcher its own separate connection,
  // documented right there: "two threads racing a BEGIN on the SAME
  // connection handle is a 'cannot start a transaction within a
  // transaction' error, not a safely-serialized one"). SQLite's own
  // serialized threading mode makes each INDIVIDUAL C API call thread-
  // safe, but does NOT make a multi-statement application-level
  // transaction atomic against another thread's calls interleaving on
  // that same connection between BEGIN and COMMIT — confirmed live by
  // tests/integration/stress_concurrency.py's same-document-path check,
  // which reproduced exactly this as a real 500
  // ({"error":"statement failed: not an error"} — SQLite's error text at
  // the point our code reads it, already stomped by another thread's
  // subsequent call on the shared connection) under concurrent writes,
  // not a hypothetical.
  //
  // An EARLIER version of this class used a SEPARATE embeddingMutex_ for
  // the embedding step's own transactions, reasoning that provider_->
  // embed() (slow — a network call for the cloud provider, real
  // inference for local) shouldn't hold the SAME lock other threads'
  // document/FTS saves need. That reasoning about embed() itself was
  // right (see upsertOne()'s own comment — it still runs fully unlocked),
  // but having TWO INDEPENDENT mutexes each individually guard their own
  // BEGIN/COMMIT against the SAME underlying connection was a real,
  // separate bug: neither mutex has any idea about the other, so a
  // thread doing a document/FTS transaction (holding only mutex_) and
  // another thread doing EmbeddingIndexer::ensureTable()'s own BEGIN
  // (holding only the old embeddingMutex_) could still race a BEGIN on
  // this same connection from two threads at once — reproduced live by
  // tests/integration/stress_embeddings_concurrency.py as the exact same
  // "cannot start a transaction within a transaction" error, just via a
  // different pair of call sites than the one the comment above already
  // documents. Fixed by consolidating onto this ONE mutex_ for every
  // BEGIN/COMMIT sequence, embedding-related or not — only embed() ITSELF
  // stays outside it. read-only queries (allIndexedPaths, rowIdForPath,
  // findPathByUuid) stay unguarded: each is a single prepare/step/destroy,
  // not a multi-statement transaction, so there's no BEGIN/COMMIT window
  // for another thread to land inside.
  std::mutex mutex_;
};

}  // namespace wikicore::index
