#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

// Owns the sqlite-vec-backed `document_embeddings` virtual table:
// creating/recreating it at the right dimensionality, and upserting or
// removing one document's vector at a time. Deliberately separate from
// IndexUpdater (which owns documents/documents_fts/tags) rather than
// folded into it — this table's very existence depends on which
// EmbeddingProvider (if any) is configured, and it can be dropped and
// recreated independently (switching providers/dimensions) without
// touching FTS or tag state at all. Only compiled in when at least one
// embedding provider is (WIKI_ENABLE_SQLITE_VEC — see root
// CMakeLists.txt); callers must guard use of this class the same way.
//
// vec0's own CREATE VIRTUAL TABLE fixes the vector dimensionality at
// creation time (a literal `float[N]` in the DDL, not a bind parameter —
// confirmed against sqlite-vec's own examples), so switching between a
// 384-dim local model and a 1536-dim cloud model means dropping and
// recreating this table, losing whatever vectors were already stored.
// That's an accepted cost, not a bug to route around: the index (this
// table included) is documented project-wide as fully disposable and
// rebuildable from the vault — see docs/architecture.md's storage-model
// section — exactly the same posture as documents_fts.
class EmbeddingIndexer {
 public:
  // db: the shared connection (see index/Database.h) — same one
  // documents/documents_fts/tags live in, not a separate file.
  explicit EmbeddingIndexer(sqlite3* db);

  // Ensures document_embeddings exists at exactly `dimensions` width AND
  // was last populated by the exact model `modelIdentifier` names — see
  // EmbeddingProvider::modelIdentifier()'s own comment for why dimensions
  // alone isn't enough (two different models can share a dimension count
  // while producing incompatible vector spaces). No-op if BOTH already
  // match. If EITHER differs (a provider/model swap since the last run),
  // drops and recreates the table empty AND clears document_embedding_state
  // (every document's content-hash tracking becomes stale too — a document
  // whose text didn't change still needs a real re-embed against the new
  // model, not a skip) — every document's embedding is lost and must be
  // recomputed, exactly like a fresh index (see class comment). Throws
  // std::runtime_error on a sqlite error.
  void ensureTable(std::size_t dimensions, const std::string& modelIdentifier);

  // Inserts or replaces the embedding for `documentRowId`. `embedding`
  // must have exactly the dimensionality ensureTable() was last called
  // with — throws std::runtime_error otherwise (a mismatched-length BLOB
  // would otherwise corrupt every future KNN query silently).
  void upsertOne(sqlite3_int64 documentRowId, const std::vector<float>& embedding);

  // No-op if no row exists for this document (matches IndexUpdater's own
  // removeOne semantics).
  void removeOne(sqlite3_int64 documentRowId);

  // The dimensionality document_embeddings currently exists at, or
  // std::nullopt if the table doesn't exist yet. Tracked via an
  // `embedding_dimensions` row in the existing index_meta key/value table
  // (see schema.h) rather than introspected from vec0's own virtual-table
  // schema — simpler and doesn't depend on a stable way to parse that
  // back out of sqlite_master.sql.
  std::optional<std::size_t> currentDimensions() const;

  // rowid,distance pairs for the `limit` closest stored embeddings to
  // `queryEmbedding`, nearest first. `queryEmbedding` must match
  // currentDimensions() — throws std::runtime_error otherwise. Empty
  // result (not an error) if document_embeddings doesn't exist yet.
  struct Neighbor {
    sqlite3_int64 documentRowId;
    double distance;
  };
  std::vector<Neighbor> nearest(const std::vector<float>& queryEmbedding, int limit) const;

  // Skip-if-unchanged + self-healing tracking (document_embedding_state,
  // see schema.h's kMigration5) — lets IndexUpdater avoid calling
  // embed() at all for a document whose content (and the configured
  // model) hasn't changed since its last SUCCESSFUL embed, while a
  // document whose last attempt FAILED keeps retrying on every
  // subsequent rescan/save with no separate "pending" state to manage.

  // True if `currentContentHash` differs from the hash recorded at this
  // document's last successful embed (or no successful embed has ever
  // happened) — i.e. embed() is actually worth calling. False means the
  // stored vector is already correct for this exact content+model.
  bool needsEmbedding(sqlite3_int64 documentRowId, const std::string& currentContentHash) const;

  // Called after a successful embed()+upsertOne() — records
  // `contentHash` as the now-current state and clears any prior error.
  void recordEmbeddingSuccess(sqlite3_int64 documentRowId, const std::string& contentHash);

  // Called after embed() (or the subsequent EmbeddingIndexer write)
  // throws — records `errorMessage` for the admin-facing status list
  // WITHOUT touching the stored content_hash, so needsEmbedding() keeps
  // returning true for this document (the hash still doesn't match
  // "no successful embed yet" / the last known-good one) until a future
  // attempt actually succeeds.
  void recordEmbeddingFailure(sqlite3_int64 documentRowId, const std::string& errorMessage);

  // Clears this document's tracking row entirely — called by
  // IndexUpdater::removeOne() alongside the vector itself; ON DELETE
  // CASCADE on document_rowid already handles the case where the
  // DOCUMENT row itself is deleted, this is for the (rarer) case of
  // clearing tracking state without deleting the document.
  void clearEmbeddingState(sqlite3_int64 documentRowId);

  struct PendingDocument {
    sqlite3_int64 documentRowId;
    std::string path;
    std::string title;
    // nullopt = never attempted yet; otherwise the error from the last
    // attempt (which is also, by construction, the CURRENT state — a
    // successful attempt would have cleared this).
    std::optional<std::string> lastError;
  };
  // Documents that need an embedding attempt right now: never attempted
  // (no document_embedding_state row at all — e.g. embeddings were just
  // enabled, or a model change cleared every row via ensureTable() above)
  // OR whose last attempt failed. Ordered by path for a stable admin UI.
  // Does NOT include documents that are simply due for a routine
  // content-hash-mismatch re-embed on next save — this is specifically
  // the "something needs a human's attention" list.
  std::vector<PendingDocument> listNeedingAttention() const;

 private:
  sqlite3* db_;
};

}  // namespace wikicore::index
