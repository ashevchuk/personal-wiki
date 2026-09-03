#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "vault/DocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

// RAII temp vault + index db per test, mirroring PathGuardTest's TempVault.
class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-docsvc-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "vault");
  }
  ~TempEnv() { fs::remove_all(root_); }

  TempEnv(const TempEnv&) = delete;
  TempEnv& operator=(const TempEnv&) = delete;

  fs::path vaultRoot() const { return root_ / "vault"; }
  fs::path dbPath() const { return root_ / "index.db"; }

 private:
  fs::path root_;
};

}  // namespace

TEST_CASE("DocumentService create/get round-trips front matter and body",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater);

  vault::DocumentInput input;
  input.title = "Systemd timers";
  input.tags = {"linux", "systemd"};
  input.visibility = "public";
  input.type = "note";
  input.body = "Some *markdown* body.\n";

  const auto created = svc.create("notes/systemd-timers.md", input);
  REQUIRE(!created.frontMatter.id.empty());
  REQUIRE(created.frontMatter.title == "Systemd timers");
  REQUIRE(created.frontMatter.visibility == "public");
  REQUIRE(!created.frontMatter.created.empty());
  REQUIRE(created.frontMatter.created == created.frontMatter.updated);

  const auto fetched = svc.get("notes/systemd-timers.md");
  REQUIRE(fetched.frontMatter.id == created.frontMatter.id);
  REQUIRE(fetched.frontMatter.tags == std::vector<std::string>{"linux", "systemd"});
  REQUIRE(fetched.body == "Some *markdown* body.\n");
}

TEST_CASE("DocumentService create rejects a path that already exists",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater);

  vault::DocumentInput input;
  input.title = "One";
  input.body = "x";
  svc.create("a.md", input);
  REQUIRE_THROWS_AS(svc.create("a.md", input), vault::DocumentAlreadyExistsError);
}

TEST_CASE("DocumentService update preserves id/created, bumps updated, "
          "rejects an unknown path",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater);

  vault::DocumentInput input;
  input.title = "Original";
  input.visibility = "private";
  input.body = "v1";
  const auto created = svc.create("a.md", input);

  vault::DocumentInput updateInput;
  updateInput.title = "Changed";
  updateInput.visibility = "public";
  updateInput.body = "v2";
  const auto updated = svc.update("a.md", updateInput);

  REQUIRE(updated.frontMatter.id == created.frontMatter.id);
  REQUIRE(updated.frontMatter.created == created.frontMatter.created);
  REQUIRE(updated.frontMatter.title == "Changed");
  REQUIRE(updated.frontMatter.visibility == "public");
  REQUIRE(updated.body == "v2");

  vault::DocumentInput irrelevant;
  REQUIRE_THROWS_AS(svc.update("does-not-exist.md", irrelevant),
                     vault::DocumentNotFoundError);
}

TEST_CASE("DocumentService update falls back to visibility=private for "
          "anything other than exactly 'public'",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater);

  vault::DocumentInput input;
  input.visibility = "PUBLIC";  // wrong case — must not slip through
  input.body = "x";
  const auto created = svc.create("a.md", input);
  REQUIRE(created.frontMatter.visibility == "private");
}

TEST_CASE("DocumentService softDelete moves the file (and assets folder) to "
          ".trash/ and removes it from the index",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater);

  vault::DocumentInput input;
  input.body = "x";
  svc.create("notes/a.md", input);
  fs::create_directories(env.vaultRoot() / "notes/a.assets");
  std::ofstream(env.vaultRoot() / "notes/a.assets/img.png") << "fake";

  svc.softDelete("notes/a.md");

  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/a.md"));
  REQUIRE(fs::exists(env.vaultRoot() / ".trash/notes/a.md"));
  REQUIRE(fs::exists(env.vaultRoot() / ".trash/notes/a.assets/img.png"));
  REQUIRE_THROWS_AS(svc.softDelete("notes/a.md"), vault::DocumentNotFoundError);
}
