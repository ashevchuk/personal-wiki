// Real end-to-end proof that FtsSearch's hybrid (BM25 + semantic) ranking
// actually finds documents plain FTS5 alone can't — a semantically
// related query with NO word in common with the stored text. Only built
// when WIKI_TEST_EMBEDDING_MODEL_PATH points at a real GGUF file (see
// tests/CMakeLists.txt) — same gate as LocalEmbeddingProviderTest.cpp /
// IndexUpdaterEmbeddingTest.cpp.
#include "embeddings/LocalEmbeddingProvider.h"
#include "index/Database.h"
#include "index/EmbeddingsRuntimeConfig.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;
using namespace wikicore::index;
using namespace wikicore::embeddings;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-fts-hybrid-test-" +
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

DocumentIndexEntry makeEntry(const std::string& path, const std::string& title,
                              const std::string& body) {
  DocumentIndexEntry e;
  e.uuid = "test-uuid-" + path;
  e.path = path;
  e.title = title;
  e.docType = "note";
  e.visibility = "public";
  e.createdAt = "2026-01-01T00:00:00Z";
  e.updatedAt = "2026-01-01T00:00:00Z";
  e.body = body;
  e.excerpt = body.substr(0, 100);
  return e;
}

bool containsPath(const std::vector<SearchResultItem>& results, const std::string& path) {
  for (const auto& r : results) {
    if (r.path == path) return true;
  }
  return false;
}

// Wraps a real LocalEmbeddingProvider, counting embed() calls — embedQuery()
// isn't overridden, so it falls through to EmbeddingProvider's own default
// (embedQuery(text) { return embed(text); }), meaning a call through
// embedQuery() increments embedCalls exactly like IndexUpdaterEmbeddingTest's
// own CountingEmbeddingProvider does for its embed()-side tests.
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

}  // namespace

TEST_CASE("FtsSearch: a plain FTS5 search (no provider) does NOT find a "
          "document via a lexically-unrelated but semantically related query "
          "— the baseline hybrid search is supposed to improve on",
          "[FtsSearch][real-model]") {
  TempDb env;
  IndexUpdater updater(env.db());  // no provider — FTS5-only, matches today's behavior
  updater.upsertOne(makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));

  FtsSearch search(env.db());  // no provider — FTS5-only
  SearchQuery q;
  q.text = "A small feline animal";  // shares zero words with the stored text
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE_FALSE(containsPath(results, "notes/cat.md"));
}

TEST_CASE("FtsSearch: hybrid search (with a real provider) DOES find that same "
          "document via the same lexically-unrelated, semantically related "
          "query — the actual proof hybrid ranking works",
          "[FtsSearch][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));
  updater.upsertOne(
      makeEntry("notes/finance.md", "Finance", "Quarterly financial report for the fiscal year."));

  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "A small feline animal";
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE(containsPath(results, "notes/cat.md"));
  // The unrelated finance document must not outrank the actually-related
  // one — proof this is real semantic ranking, not "return everything".
  REQUIRE(results.front().path == "notes/cat.md");
}

TEST_CASE("FtsSearch: hybrid search still finds an exact keyword match — "
          "adding semantic ranking must not break plain lexical search",
          "[FtsSearch][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(
      makeEntry("notes/systemd.md", "Systemd", "systemd timers are great for scheduling."));
  updater.upsertOne(makeEntry("notes/unrelated.md", "Unrelated", "A recipe for banana bread."));

  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "systemd timers";  // exact words present in the target document
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE(containsPath(results, "notes/systemd.md"));
  REQUIRE(results.front().path == "notes/systemd.md");
}

TEST_CASE("FtsSearch: hybrid search with no document_embeddings table yet "
          "(provider set, but nothing indexed) falls back to plain FTS5 "
          "instead of returning nothing",
          "[FtsSearch][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  // Indexed WITHOUT a provider — document_embeddings never gets created.
  IndexUpdater updater(env.db());
  updater.upsertOne(makeEntry("notes/a.md", "A", "systemd timers are useful."));

  // Search WITH a provider configured, but the table doesn't exist.
  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "systemd timers";
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE(containsPath(results, "notes/a.md"));  // FTS5 fallback still works
}

TEST_CASE("FtsSearch: the runtime embeddings_runtime_config toggle actually "
          "turns hybrid ranking off, live, with no restart involved — same "
          "provider instance, same FtsSearch instance, just the DB flag "
          "flipped between calls",
          "[FtsSearch][real-model]") {
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));
  updater.upsertOne(
      makeEntry("notes/finance.md", "Finance", "Quarterly financial report for the fiscal year."));

  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "A small feline animal";  // shares zero words with the stored text
  q.includePrivate = true;

  // Enabled (the default) — semantic ranking finds it, same as the plain
  // hybrid test above.
  REQUIRE(containsPath(search.search(q), "notes/cat.md"));

  // Flip the toggle off — the SAME FtsSearch instance, SAME provider, same
  // query, must now fall back to FTS5-only and miss the lexically-unrelated
  // match, proving search() actually re-checks the flag per call rather
  // than caching a decision made at construction time.
  EmbeddingsRuntimeConfig(env.db()).setVectorSearchEnabled(false);
  REQUIRE_FALSE(containsPath(search.search(q), "notes/cat.md"));

  // Flip it back on — proves this isn't a one-way trip, an admin can
  // re-enable it live too.
  EmbeddingsRuntimeConfig(env.db()).setVectorSearchEnabled(true);
  REQUIRE(containsPath(search.search(q), "notes/cat.md"));
}

TEST_CASE("FtsSearch: a genuinely unrelated document does NOT appear in "
          "hybrid results just for being the closest thing available in a "
          "small vault",
          "[FtsSearch][real-model]") {
  // Real bug, found live from a user report on real production content:
  // EmbeddingIndexer::nearest() has no relevance floor of its own — on a
  // small vault, "the nearest N neighbors" is effectively the WHOLE
  // vault, ranked by a distance that's often pure noise for a genuinely
  // unrelated document, and RRF gave every one of them a nonzero score
  // regardless of actual relevance. A one-word query against a handful of
  // documents returned recipes, a welcome page, and empty demo docs
  // alongside the couple of actually-relevant results. See
  // docs/embeddings.md's "hybrid search relevance" writeup for the real
  // measured numbers behind the fix (a query-side instruction prefix,
  // via embedQuery(), plus a configurable cosine-distance cutoff in
  // FtsSearch itself).
  TempDb env;
  LocalEmbeddingProvider provider(
      WIKI_TEST_EMBEDDING_MODEL_PATH,
      "Represent this sentence for searching relevant passages: ");
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(makeEntry("notes/stability.md", "API Stability",
                               "Once a public interface stabilizes, avoid breaking changes."));
  updater.upsertOne(
      makeEntry("recipes/borscht.md", "Borscht",
                "Beets, cabbage, potato, carrot, onion. Simmer broth with beef."));
  updater.upsertOne(makeEntry("welcome.md", "Welcome", "Welcome to the wiki."));

  // Default maxSemanticDistance (0.5, same as AppConfig::embeddingsMaxDistance's
  // own default) — deliberately NOT overridden, so this test exercises the
  // real default an admin gets without touching config.toml.
  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "stabilize";
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE(containsPath(results, "notes/stability.md"));
  REQUIRE_FALSE(containsPath(results, "recipes/borscht.md"));
  REQUIRE_FALSE(containsPath(results, "welcome.md"));
}

TEST_CASE("FtsSearch: maxSemanticCandidates caps how many semantic matches "
          "flood in, even when every single one is individually under the "
          "distance cutoff",
          "[FtsSearch][real-model]") {
  // A second, distinct real bug found re-checking the distance-cutoff fix
  // above against more real production queries: a MULTI-word query's
  // embedding can sit at a uniformly "blurry" distance from several
  // unrelated documents at once on a small vault — each one individually
  // still under maxSemanticDistance, so the cutoff alone lets all of them
  // through. RRF has no way to reject a candidate once it's admitted to a
  // ranked list, only rank it (see FtsSearch.h's own comment) — so the fix
  // has to happen at admission, via a hard count cap on top of the
  // distance cutoff, not instead of it. See docs/embeddings.md.
  TempDb env;
  LocalEmbeddingProvider provider(
      WIKI_TEST_EMBEDDING_MODEL_PATH,
      "Represent this sentence for searching relevant passages: ");
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));
  updater.upsertOne(
      makeEntry("notes/finance.md", "Finance", "Quarterly financial report for the fiscal year."));
  updater.upsertOne(makeEntry("recipes/borscht.md", "Borscht",
                               "Beets, cabbage, potato, carrot, onion. Simmer broth with beef."));
  updater.upsertOne(
      makeEntry("notes/systemd.md", "Systemd", "systemd timers are great for scheduling."));
  updater.upsertOne(makeEntry("notes/move.md", "Move Semantics",
                               "std::move casts an lvalue to an rvalue reference."));

  // maxSemanticDistance deliberately wide open (cosine distance never
  // exceeds 2.0) so every one of the 5 documents above is a valid semantic
  // candidate on distance alone — isolates the count cap as the only thing
  // doing any filtering here, same isolation technique as the "stabilize"
  // test above isolates the distance cutoff by using its own default.
  FtsSearch search(env.db(), &provider, /*maxSemanticDistance=*/1.9,
                    /*maxSemanticCandidates=*/2);
  SearchQuery q;
  q.text = "A small feline animal";  // shares zero words with any stored text —
                                      // BM25 contributes nothing, isolating the
                                      // semantic side exactly like the earlier
                                      // "cat" tests above.
  q.includePrivate = true;
  const auto results = search.search(q);

  REQUIRE(results.size() == 2);
  // The one genuinely relevant document must still be the nearest
  // neighbor and survive the cap.
  REQUIRE(containsPath(results, "notes/cat.md"));
}

TEST_CASE("FtsSearch: an active tag filter excludes a document that only "
          "matches through the SEMANTIC candidate path, not just the BM25 "
          "one",
          "[FtsSearch][real-model]") {
  // Real bug, reported live: searching "soup" with tags:cpp + type:note
  // filters active still found a genuinely unrelated recipe document.
  // Root cause: EmbeddingIndexer::nearest() (the semantic candidate
  // source) has no concept of tag/docType/folder filters at all —
  // bm25CandidateRowIds() applied them correctly, but a document
  // admitted ONLY through the semantic side skipped the filter entirely,
  // and fetchByRowIds() — where both candidate sources converge before
  // being returned to the caller — never re-checked it either. See
  // FtsSearch.h's own comment on fetchByRowIds() for the full writeup.
  TempDb env;
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  IndexUpdater updater(env.db(), &provider);

  auto soup = makeEntry("recipes/soup.md", "Soup",
                         "A hearty vegetable soup recipe for cold days.");
  soup.tags = {"python"};
  updater.upsertOne(soup);

  FtsSearch search(env.db(), &provider);

  // Baseline: no filter — the soup document IS findable (proves this
  // isn't a "the model can't find soup" test artifact).
  SearchQuery unfiltered;
  unfiltered.text = "soup";
  unfiltered.includePrivate = true;
  REQUIRE(containsPath(search.search(unfiltered), "recipes/soup.md"));

  // With an ACTIVE tags:cpp filter — the document isn't tagged cpp, so
  // it must NOT appear, regardless of how strongly "soup" matches it
  // semantically or lexically.
  SearchQuery filtered;
  filtered.text = "soup";
  filtered.tag = "cpp";
  filtered.includePrivate = true;
  REQUIRE_FALSE(containsPath(search.search(filtered), "recipes/soup.md"));
}

TEST_CASE("FtsSearch: a repeated identical query does NOT call embedQuery() "
          "again — the whole point of caching it",
          "[FtsSearch][real-model]") {
  // Found live: a single embedQuery() call measured 1.3-2.6 SECONDS on the
  // real production armv7 SBC — by far the dominant cost of a hybrid
  // search request. search.js's tag/type filter checkboxes re-run a
  // search with the SAME query text on every toggle, with no debounce —
  // a real, common case this cache is meant to fix. See FtsSearch.h's own
  // comment on queryEmbeddingCache_ for the full reasoning.
  TempDb env;
  LocalEmbeddingProvider realProvider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  CountingEmbeddingProvider provider(realProvider);
  IndexUpdater updater(env.db(), &provider);
  updater.upsertOne(makeEntry("notes/cat.md", "About cats", "The cat sat on the mat."));

  FtsSearch search(env.db(), &provider);
  SearchQuery q;
  q.text = "A small feline animal";
  q.includePrivate = true;

  provider.embedCalls = 0;  // upsertOne() above already embedded the document once

  search.search(q);
  REQUIRE(provider.embedCalls == 1);

  search.search(q);
  REQUIRE(provider.embedCalls == 1);  // second identical query: cache hit, no new call

  SearchQuery different;
  different.text = "Quarterly financial report";
  different.includePrivate = true;
  search.search(different);
  REQUIRE(provider.embedCalls == 2);  // different text: genuinely not cached yet
}
