#include "embeddings/EmbeddingChunks.h"
#include "embeddings/EmbeddingProvider.h"
#include "index/Database.h"
#include "index/EmbeddingIndexer.h"
#include "index/IndexUpdater.h"
#include "index/Statement.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace fs = std::filesystem;
using namespace wikicore;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-chunk-progress-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")) {
    fs::remove(path_);
    db_ = std::make_unique<Database>(path_);
    db_->migrate();
  }
  ~TempDb() {
    db_.reset();
    fs::remove(path_);
    fs::remove(path_.string() + "-wal");
    fs::remove(path_.string() + "-shm");
  }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  Database& db() { return *db_; }

 private:
  fs::path path_;
  std::unique_ptr<Database> db_;
};

// Deterministic 4-dimensional vectors derived from the text; `onEmbed`
// runs after each call, on the embedding worker thread.
class FakeProvider : public embeddings::EmbeddingProvider {
 public:
  explicit FakeProvider(std::string model = "fake-model") : model_(std::move(model)) {}

  std::vector<float> embed(const std::string& text) override {
    const auto h = static_cast<float>(std::hash<std::string>{}(text) % 1000);
    const int call = ++calls;
    if (onEmbed) onEmbed(call);
    return {h, 1.0f, 2.0f, static_cast<float>(text.size() % 97)};
  }
  std::size_t dimensions() const override { return 4; }
  std::string modelIdentifier() const override { return model_; }

  std::atomic<int> calls{0};
  std::function<void(int)> onEmbed;

 private:
  std::string model_;
};

const std::string kTitle = "Long note";

std::string longBody() {
  std::string body;
  for (int i = 0; i < 400; ++i) body += "word" + std::to_string(i) + " lorem ipsum ";
  return body;
}

DocumentIndexEntry makeEntry(const std::string& path, const std::string& body) {
  DocumentIndexEntry entry;
  entry.uuid = "uuid-" + path;
  entry.path = path;
  entry.title = kTitle;
  entry.docType = "note";
  entry.visibility = "public";
  entry.createdAt = "2026-01-01T00:00:00Z";
  entry.updatedAt = "2026-01-01T00:00:00Z";
  entry.body = body;
  return entry;
}

int progressRows(Database& db, int64_t rowId) {
  Statement stmt(db.handle(),
                 "SELECT COUNT(*) FROM embedding_chunk_progress WHERE document_rowid = ?1;");
  stmt.bind(1, rowId);
  stmt.step();
  return static_cast<int>(stmt.columnInt64(0));
}

}  // namespace

TEST_CASE("An abandoned embedding pass keeps finished chunks and the next pass "
          "embeds only the rest",
          "[IndexUpdater][EmbeddingIndexer]") {
  TempDb env;
  const std::string body = longBody();
  const int totalChunks =
      static_cast<int>(embeddings::chunkForEmbedding(kTitle, body).size());
  REQUIRE(totalChunks > 4);
  constexpr int kChunksBeforeShutdown = 3;

  int64_t rowId = 0;
  {
    FakeProvider provider;
    IndexUpdater updater(env.db(), &provider);
    provider.onEmbed = [&updater](int call) {
      if (call == kChunksBeforeShutdown) updater.abandonPendingEmbeddings();
    };
    rowId = updater.upsertOne(makeEntry("notes/long.md", body));
    updater.flushEmbeddings();
  }

  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE(progressRows(env.db(), rowId) == kChunksBeforeShutdown);
  REQUIRE(indexer.chunkCount(rowId) == 0);

  FakeProvider provider;
  {
    IndexUpdater updater(env.db(), &provider);
    REQUIRE(updater.upsertOne(makeEntry("notes/long.md", body)) == rowId);
    updater.flushEmbeddings();
  }
  REQUIRE(provider.calls == totalChunks - kChunksBeforeShutdown);
  REQUIRE(indexer.chunkCount(rowId) == totalChunks);
  REQUIRE(indexer.listNeedingAttention().empty());
  REQUIRE(progressRows(env.db(), rowId) == 0);
}

TEST_CASE("abandonPendingEmbeddings returns flushEmbeddings at once and drops "
          "queued jobs while a chunk is still being embedded",
          "[IndexUpdater][EmbeddingIndexer]") {
  TempDb env;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::promise<void> firstCallStarted;

  FakeProvider provider;
  provider.onEmbed = [&](int call) {
    if (call == 1) {
      firstCallStarted.set_value();
      released.wait();
    }
  };

  std::optional<IndexUpdater> updater(std::in_place, env.db(), &provider);
  const int64_t first = updater->upsertOne(makeEntry("notes/a.md", longBody()));
  firstCallStarted.get_future().wait();
  const int64_t second = updater->upsertOne(makeEntry("notes/b.md", longBody() + " extra"));

  updater->abandonPendingEmbeddings();
  auto flushed = std::async(std::launch::async, [&] { updater->flushEmbeddings(); });
  REQUIRE(flushed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

  release.set_value();
  updater.reset();

  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE(provider.calls == 1);
  REQUIRE(indexer.chunkCount(first) == 0);
  REQUIRE(indexer.chunkCount(second) == 0);
  REQUIRE(progressRows(env.db(), first) == 1);
  REQUIRE(progressRows(env.db(), second) == 0);
}

TEST_CASE("A model change discards stored chunk progress", "[EmbeddingIndexer]") {
  TempDb env;
  FakeProvider provider;
  int64_t rowId = 0;
  {
    IndexUpdater updater(env.db(), &provider);
    provider.onEmbed = [&updater](int call) {
      if (call == 2) updater.abandonPendingEmbeddings();
    };
    rowId = updater.upsertOne(makeEntry("notes/long.md", longBody()));
    updater.flushEmbeddings();
  }
  REQUIRE(progressRows(env.db(), rowId) == 2);

  EmbeddingIndexer(env.db().handle()).ensureTable(4, "another-model");
  REQUIRE(progressRows(env.db(), rowId) == 0);
}
