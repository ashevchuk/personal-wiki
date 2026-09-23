#include "index/AgentChatStore.h"
#include "index/Database.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-agent-chat-store-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       ".db")) {
    fs::remove(path_);
  }
  ~TempDb() {
    std::error_code ec;
    fs::remove(path_, ec);
    fs::remove(fs::path(path_.string() + "-wal"), ec);
    fs::remove(fs::path(path_.string() + "-shm"), ec);
  }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

AgentChatRecord row(const std::string& id, const std::string& title,
                    const std::string& updated) {
  AgentChatRecord r;
  r.id = id;
  r.title = title;
  r.createdAt = "2026-01-01T00:00:00Z";
  r.updatedAt = updated;
  r.eventsJson = R"([{"type":"user","data":{"text":"hi"}}])";
  r.messagesJson = R"([{"role":"user","content":"hi"}])";
  return r;
}

}  // namespace

TEST_CASE("AgentChatStore upserts, lists newest first, renames, and removes",
          "[AgentChatStore]") {
  TempDb tmp;
  Database db(tmp.path());
  db.migrate();
  AgentChatStore store(db);

  store.upsert(row("older", "Older", "2026-01-01T00:00:01Z"));
  store.upsert(row("newer", "Newer", "2026-01-01T00:00:02Z"));

  const auto listed = store.list();
  REQUIRE(listed.size() == 2);
  REQUIRE(listed[0].id == "newer");
  REQUIRE(listed[0].title == "Newer");
  REQUIRE(listed[1].id == "older");

  auto got = store.get("older");
  REQUIRE(got);
  REQUIRE(got->eventsJson.find("user") != std::string::npos);

  store.rename("older", "Renamed");
  got = store.get("older");
  REQUIRE(got);
  REQUIRE(got->title == "Renamed");

  store.upsert(row("older", "Renamed", "2026-01-01T00:00:03Z"));
  REQUIRE(store.list()[0].id == "older");

  store.remove("newer");
  REQUIRE_FALSE(store.get("newer"));
  REQUIRE(store.list().size() == 1);

  bool threw = false;
  try {
    store.rename("missing", "Nope");
  } catch (const std::runtime_error& e) {
    threw = true;
    REQUIRE(std::string(e.what()) == "session not found");
  }
  REQUIRE(threw);
}
