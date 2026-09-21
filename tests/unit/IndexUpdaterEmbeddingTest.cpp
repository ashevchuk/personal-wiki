// Real end-to-end proof that IndexUpdater::upsertOne() actually computes
// and stores an embedding when a real EmbeddingProvider is wired in — not
// just that EmbeddingIndexer works in isolation (EmbeddingIndexerTest.cpp)
// or that LocalEmbeddingProvider produces sane vectors in isolation
// (LocalEmbeddingProviderTest.cpp). Only built when
// WIKI_TEST_EMBEDDING_MODEL_PATH points at a real GGUF file (see
// tests/CMakeLists.txt) — same gate as LocalEmbeddingProviderTest.cpp.
#include "embeddings/LocalEmbeddingProvider.h"
#include "index/Database.h"
#include "index/EmbeddingIndexer.h"
#include "index/IndexUpdater.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace wikicore;
using namespace wikicore::index;
using namespace wikicore::embeddings;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-indexupdater-embedding-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")) {
    fs::remove(path_);
    db_ = std::make_unique<Database>(path_);
    db_->migrate();
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  Database& db() { return *db_; }

 private:
  fs::path path_;
  std::unique_ptr<Database> db_;
};

// Wraps a real LocalEmbeddingProvider and counts embed() calls — used to
// prove the skip-if-unchanged logic in IndexUpdater::upsertOne() actually
// prevents a real re-embed, not just that EmbeddingIndexer::needsEmbedding()
// returns the right bool in isolation (already covered by
// EmbeddingIndexerTest.cpp).
class CountingEmbeddingProvider : public EmbeddingProvider {
 public:
  explicit CountingEmbeddingProvider(LocalEmbeddingProvider& inner) : inner_(inner) {}

  std::vector<float> embed(const std::string& text) override {
    ++embedCalls;
    return inner_.embed(text);
  }
  std::size_t dimensions() const override { return inner_.dimensions(); }
  std::string modelIdentifier() const override { return inner_.modelIdentifier(); }

  int embedCalls = 0;

 private:
  LocalEmbeddingProvider& inner_;
};

DocumentIndexEntry makeEntry(const std::string& path, const std::string& title,
                              const std::string& body) {
  DocumentIndexEntry entry;
  entry.uuid = "test-uuid-" + path;
  entry.path = path;
  entry.title = title;
  entry.docType = "note";
  entry.visibility = "public";
  entry.createdAt = "2026-01-01T00:00:00Z";
  entry.updatedAt = "2026-01-01T00:00:00Z";
  entry.body = body;
  return entry;
}

}  // namespace

TEST_CASE("IndexUpdater::upsertOne, with a real provider wired in, actually "
          "populates document_embeddings — not just documents_fts",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater indexUpdater(env.db(), &provider);

  const int64_t rowId = indexUpdater.upsertOne(
      makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));
  indexUpdater.flushEmbeddings();

  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE(indexer.currentDimensions() == provider.dimensions());

  // Confirm via a real nearest() query, not by reaching into internals —
  // this document must be findable by its own embedding. Query text must
  // match EXACTLY what IndexUpdater::upsertOne() itself embeds
  // (entry.title + "\n\n" + entry.body, see IndexUpdater.cpp) — querying
  // with the body alone was tried first and genuinely failed
  // (distance ~0.05, not ~0): a real, if unsurprising, empirical finding
  // that title+body and body-alone embed to meaningfully different
  // vectors on this model, not test flakiness to paper over with a
  // looser threshold.
  const auto self = provider.embed("About cats\n\nThe cat sat on the mat.");
  const auto results = indexer.nearest(self, 1);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].documentRowId == rowId);
  REQUIRE(results[0].distance < 0.01);  // itself — should be ~0 distance
}

TEST_CASE("IndexUpdater::upsertOne on an UPDATE re-embeds the new body, not "
          "the old one",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater indexUpdater(env.db(), &provider);

  const int64_t rowId = indexUpdater.upsertOne(
      makeEntry("notes/topic.md", "Topic", "The cat sat on the mat."));
  indexUpdater.upsertOne(
      makeEntry("notes/topic.md", "Topic", "Quarterly financial report for the fiscal year."));
  indexUpdater.flushEmbeddings();

  EmbeddingIndexer indexer(env.db().handle());
  const auto catQuery = provider.embed("The cat sat on the mat.");
  const auto financeQuery = provider.embed("Quarterly financial report for the fiscal year.");

  const auto nearCat = indexer.nearest(catQuery, 1);
  const auto nearFinance = indexer.nearest(financeQuery, 1);

  // After the update, the stored vector should be much closer to the
  // financial-report query than to the original cat sentence — proof the
  // update recomputed the embedding rather than keeping the stale one.
  REQUIRE(nearFinance[0].documentRowId == rowId);
  REQUIRE(nearFinance[0].distance < nearCat[0].distance);
}

TEST_CASE("IndexUpdater::removeOne also removes the row's embedding, not "
          "just its FTS entry",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater indexUpdater(env.db(), &provider);

  // Well above the default minEmbeddingWords_ (6) — this test is about
  // removeOne() cleaning up a REAL embedding, not about the separate
  // too-short-to-embed skip (see the dedicated test cases below for that).
  const std::string body = "Some temporary content that will be removed again shortly.";
  indexUpdater.upsertOne(makeEntry("notes/temp.md", "Temp", body));
  indexUpdater.flushEmbeddings();
  indexUpdater.removeOne("notes/temp.md");

  EmbeddingIndexer indexer(env.db().handle());
  const auto query = provider.embed(body);
  REQUIRE(indexer.nearest(query, 10).empty());
}

TEST_CASE("IndexUpdater with NO provider (nullptr) never touches "
          "document_embeddings — the embedding step is opt-in, not "
          "unconditional",
          "[IndexUpdater][EmbeddingIndexer]") {
  TempDb env;
  IndexUpdater indexUpdater(env.db());  // no provider — default nullptr

  indexUpdater.upsertOne(makeEntry("notes/a.md", "A", "content"));

  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE_FALSE(indexer.currentDimensions().has_value());  // table never created
}

TEST_CASE("IndexUpdater::upsertOne skips a real embed() call when content is "
          "unchanged, and still re-embeds when it genuinely changes",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider realProvider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  CountingEmbeddingProvider provider(realProvider);
  IndexUpdater indexUpdater(env.db(), &provider);

  indexUpdater.upsertOne(makeEntry("notes/skip.md", "Skip", "The cat sat on the mat."));
  indexUpdater.flushEmbeddings();
  REQUIRE(provider.embedCalls == 1);

  // Same path, same title, same body — a real save with nothing actually
  // changed (e.g. a front-matter-only touch upstream, or a plain re-save).
  indexUpdater.upsertOne(makeEntry("notes/skip.md", "Skip", "The cat sat on the mat."));
  indexUpdater.flushEmbeddings();
  REQUIRE(provider.embedCalls == 1);  // no second real embed() call

  // Genuinely different body — must trigger a real re-embed, proving the
  // skip above wasn't a false positive that ignores content changes too.
  indexUpdater.upsertOne(
      makeEntry("notes/skip.md", "Skip", "Quarterly financial report for the fiscal year."));
  indexUpdater.flushEmbeddings();
  REQUIRE(provider.embedCalls == 2);
}

TEST_CASE("IndexUpdater::upsertOne never calls embed() at all for a "
          "too-short document, and doesn't flag it as needing attention",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  // Real bug, found live from a user report on real production content: a
  // near-empty document (title plus just an image link, no real prose)
  // produced a vector whose semantic signal was too weak to reliably land
  // far from unrelated queries — no distance threshold alone fixes a
  // document whose OWN vector doesn't carry a strong enough signal. See
  // docs/embeddings.md's "A real relevance bug" for the measured numbers.
  TempDb env;
  LocalEmbeddingProvider realProvider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  CountingEmbeddingProvider provider(realProvider);
  // Default minEmbeddingWords_ (6) — "Welcome" (1) + "Hi there." (2) = 3
  // words, well under it.
  IndexUpdater indexUpdater(env.db(), &provider);

  const int64_t rowId =
      indexUpdater.upsertOne(makeEntry("welcome.md", "Welcome", "Hi there."));
  REQUIRE(provider.embedCalls == 0);  // embed() never called at all

  EmbeddingIndexer indexer(env.db().handle());
  const auto needing = indexer.listNeedingAttention();
  const bool flagged =
      std::any_of(needing.begin(), needing.end(),
                  [rowId](const auto& doc) { return doc.documentRowId == rowId; });
  REQUIRE_FALSE(flagged);  // skipped-as-too-short is NOT "needs attention"
}

TEST_CASE("IndexUpdater::upsertOne removes a STALE embedding when an edit "
          "shrinks a document below the too-short threshold",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater indexUpdater(env.db(), &provider);

  const std::string longBody = "This document originally had enough real content to embed.";
  const int64_t rowId =
      indexUpdater.upsertOne(makeEntry("notes/shrinking.md", "Shrinking Doc", longBody));
  indexUpdater.flushEmbeddings();

  EmbeddingIndexer indexer(env.db().handle());
  const auto queryBeforeShrink = provider.embed(longBody);
  const auto beforeResults = indexer.nearest(queryBeforeShrink, 10);
  REQUIRE(std::any_of(beforeResults.begin(), beforeResults.end(),
                       [rowId](const auto& n) { return n.documentRowId == rowId; }));

  // Edited down to just a couple of words — the stale vector from the
  // longer version must not silently keep influencing semantic search.
  indexUpdater.upsertOne(makeEntry("notes/shrinking.md", "Shrinking Doc", "Too short now."));
  const auto afterResults = indexer.nearest(queryBeforeShrink, 10);
  REQUIRE_FALSE(std::any_of(afterResults.begin(), afterResults.end(),
                             [rowId](const auto& n) { return n.documentRowId == rowId; }));
}

TEST_CASE("IndexUpdater::upsertOne self-heals: a document edited to finally "
          "have enough real content gets a real embed() on that save",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  TempDb env;
  LocalEmbeddingProvider realProvider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  CountingEmbeddingProvider provider(realProvider);
  IndexUpdater indexUpdater(env.db(), &provider);

  indexUpdater.upsertOne(makeEntry("notes/growing.md", "Stub", "Too short."));
  REQUIRE(provider.embedCalls == 0);

  const int64_t rowId = indexUpdater.upsertOne(makeEntry(
      "notes/growing.md", "Stub", "Now this document has grown enough real prose to embed."));
  indexUpdater.flushEmbeddings();
  REQUIRE(provider.embedCalls == 1);

  EmbeddingIndexer indexer(env.db().handle());
  const auto query =
      realProvider.embed("Now this document has grown enough real prose to embed.");
  const auto results = indexer.nearest(query, 10);
  REQUIRE(std::any_of(results.begin(), results.end(),
                       [rowId](const auto& n) { return n.documentRowId == rowId; }));
}

TEST_CASE("IndexUpdater::upsertOne: a too-short document processed BEFORE "
          "any real embed doesn't lose its recorded state once a real "
          "embed happens afterward",
          "[IndexUpdater][EmbeddingIndexer][real-model]") {
  // Real bug, found live wiring up the too-short-document skip itself (a
  // genuine "the fix had a bug" moment, caught before shipping, not a
  // hypothetical): the skip path originally recorded success WITHOUT
  // calling EmbeddingIndexer::ensureTable() first. If a too-short
  // document is the FIRST one ever processed, index_meta's dimensions/
  // model are still completely unset at that point. The next REAL
  // document's own ensureTable() call then sees "no recorded dimensions"
  // as a model MISMATCH — the exact same signal a genuine model swap
  // gives — and wipes the ENTIRE document_embedding_state table as a
  // side effect of establishing the schema, silently erasing the
  // too-short document's just-recorded row. Fixed by having the skip
  // path call ensureTable() too (cheap — a no-op once already matching).
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater indexUpdater(env.db(), &provider);

  // Too-short document FIRST — nothing else has called ensureTable() yet.
  const int64_t shortRowId =
      indexUpdater.upsertOne(makeEntry("welcome.md", "Welcome", "Hi there."));

  // A real, normal-length document SECOND — this is the one whose own
  // ensureTable() call used to wipe the row just written above.
  indexUpdater.upsertOne(makeEntry(
      "notes/real.md", "Real Document", "This document has plenty of real content to embed."));
  indexUpdater.flushEmbeddings();

  EmbeddingIndexer indexer(env.db().handle());
  const auto needing = indexer.listNeedingAttention();
  const bool shortDocFlagged =
      std::any_of(needing.begin(), needing.end(),
                  [shortRowId](const auto& doc) { return doc.documentRowId == shortRowId; });
  REQUIRE_FALSE(shortDocFlagged);
}
