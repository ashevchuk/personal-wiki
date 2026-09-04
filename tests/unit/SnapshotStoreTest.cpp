#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-snapshot-test-" +
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

DocumentIndexEntry makeEntry(std::string path) {
  DocumentIndexEntry e;
  e.uuid = path;
  e.path = std::move(path);
  e.title = e.path;
  e.visibility = "public";
  e.createdAt = e.updatedAt = "2026-01-01T00:00:00Z";
  return e;
}

}  // namespace

TEST_CASE("SnapshotStore::list is newest first", "[SnapshotStore]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md"));
  const auto rowId = *updater.rowIdForPath("a.md");

  SnapshotStore snapshots(database);
  snapshots.record(rowId, "content v1");
  snapshots.record(rowId, "content v2");
  snapshots.record(rowId, "content v3");

  const auto list = snapshots.list(rowId);
  REQUIRE(list.size() == 3);
  REQUIRE(*snapshots.getContent(rowId, list[0].id) == "content v3");
  REQUIRE(*snapshots.getContent(rowId, list[2].id) == "content v1");
}

TEST_CASE("SnapshotStore::getContent refuses a snapshot id that belongs to "
          "a DIFFERENT document (not just any nonexistent id)",
          "[SnapshotStore]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md"));
  updater.upsertOne(makeEntry("b.md"));
  const auto rowA = *updater.rowIdForPath("a.md");
  const auto rowB = *updater.rowIdForPath("b.md");

  SnapshotStore snapshots(database);
  snapshots.record(rowA, "a's secret content");
  const auto aSnapshotId = snapshots.list(rowA)[0].id;

  // The id is real -- it's just not b's. A caller who can see b's
  // history must not be able to read a's snapshot content by asking for
  // (rowB, aSnapshotId) -- this is the exact IDOR shape the header
  // comment on getContent describes.
  REQUIRE_FALSE(snapshots.getContent(rowB, aSnapshotId).has_value());
  // The correct pairing still works.
  REQUIRE(*snapshots.getContent(rowA, aSnapshotId) == "a's secret content");
}

TEST_CASE("SnapshotStore::getContent on a completely nonexistent id returns "
          "nullopt, not an error",
          "[SnapshotStore]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md"));
  const auto rowId = *updater.rowIdForPath("a.md");

  SnapshotStore snapshots(database);
  REQUIRE_FALSE(snapshots.getContent(rowId, 999999).has_value());
}
