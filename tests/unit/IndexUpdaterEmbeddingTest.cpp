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

  indexUpdater.upsertOne(makeEntry("notes/temp.md", "Temp", "Some temporary content."));
  indexUpdater.removeOne("notes/temp.md");

  EmbeddingIndexer indexer(env.db().handle());
  const auto query = provider.embed("Some temporary content.");
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
  REQUIRE(provider.embedCalls == 1);

  // Same path, same title, same body — a real save with nothing actually
  // changed (e.g. a front-matter-only touch upstream, or a plain re-save).
  indexUpdater.upsertOne(makeEntry("notes/skip.md", "Skip", "The cat sat on the mat."));
  REQUIRE(provider.embedCalls == 1);  // no second real embed() call

  // Genuinely different body — must trigger a real re-embed, proving the
  // skip above wasn't a false positive that ignores content changes too.
  indexUpdater.upsertOne(
      makeEntry("notes/skip.md", "Skip", "Quarterly financial report for the fiscal year."));
  REQUIRE(provider.embedCalls == 2);
}
