#include "index/Database.h"
#include "index/EmbeddingsRuntimeConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace wikicore;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-embeddings-runtime-config-test-" +
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

}  // namespace

TEST_CASE("EmbeddingsRuntimeConfig defaults to enabled before any admin ever "
          "touches the toggle",
          "[EmbeddingsRuntimeConfig]") {
  TempDb env;
  EmbeddingsRuntimeConfig config(env.db());
  REQUIRE(config.isVectorSearchEnabled());
}

TEST_CASE("EmbeddingsRuntimeConfig persists an explicit disable/re-enable",
          "[EmbeddingsRuntimeConfig]") {
  TempDb env;
  EmbeddingsRuntimeConfig config(env.db());

  config.setVectorSearchEnabled(false);
  REQUIRE_FALSE(config.isVectorSearchEnabled());

  config.setVectorSearchEnabled(true);
  REQUIRE(config.isVectorSearchEnabled());
}

TEST_CASE("EmbeddingsRuntimeConfig toggle survives across separate instances "
          "against the same connection — proof it's really SQLite-backed, "
          "not held in memory",
          "[EmbeddingsRuntimeConfig]") {
  TempDb env;
  EmbeddingsRuntimeConfig(env.db()).setVectorSearchEnabled(false);

  EmbeddingsRuntimeConfig freshHandle(env.db());
  REQUIRE_FALSE(freshHandle.isVectorSearchEnabled());
}
