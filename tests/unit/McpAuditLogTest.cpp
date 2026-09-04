#include "index/Database.h"
#include "index/McpAuditLog.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-audit-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".db")) {
    fs::remove(path_);
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST_CASE("McpAuditLog::listRecent is newest first and keeps failed "
          "attempts, not just successful ones",
          "[McpAuditLog]") {
  TempDb db;
  Database database(db.path());
  database.migrate();

  McpAuditLog log(database);
  log.record("create_document", "notes/a.md", true, "created");
  log.record("create_document", "notes/a.md", false, "a document already exists at that path");
  log.record("update_document", "notes/b.md", true, "updated");

  const auto entries = log.listRecent(10);
  REQUIRE(entries.size() == 3);
  // Newest first.
  REQUIRE(entries[0].toolName == "update_document");
  REQUIRE(entries[0].success);
  REQUIRE(entries[1].toolName == "create_document");
  REQUIRE_FALSE(entries[1].success);
  REQUIRE(entries[1].detail == "a document already exists at that path");
  REQUIRE(entries[2].success);
}

TEST_CASE("McpAuditLog::listRecent respects the limit", "[McpAuditLog]") {
  TempDb db;
  Database database(db.path());
  database.migrate();

  McpAuditLog log(database);
  for (int i = 0; i < 5; ++i) {
    log.record("create_document", "notes/" + std::to_string(i) + ".md", true, "created");
  }

  REQUIRE(log.listRecent(2).size() == 2);
  REQUIRE(log.listRecent(100).size() == 5);
}
