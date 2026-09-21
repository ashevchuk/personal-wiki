#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "vault/DocumentService.h"
#include "vault/McpUploadStaging.h"
#include "vault/VaultRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-mcp-upload-test-" +
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

TEST_CASE("McpUploadStaging begin/get round-trips path and filename",
          "[McpUploadStaging]") {
  TempEnv env;
  vault::VaultRepository repo(env.vaultRoot());
  vault::McpUploadStaging staging(repo);

  const std::string id = staging.begin("notes/a.md", "paper.pdf");
  REQUIRE(vault::McpUploadStaging::isValidId(id));
  const auto ticket = staging.get(id);
  REQUIRE(ticket.has_value());
  REQUIRE(ticket->documentPath == "notes/a.md");
  REQUIRE(ticket->filename == "paper.pdf");
}

TEST_CASE("McpUploadStaging::get rejects a non-uuid and a missing id",
          "[McpUploadStaging]") {
  TempEnv env;
  vault::VaultRepository repo(env.vaultRoot());
  vault::McpUploadStaging staging(repo);

  REQUIRE_FALSE(staging.get("../etc/passwd").has_value());
  REQUIRE_FALSE(staging.get("not-a-uuid").has_value());
  REQUIRE_FALSE(staging.get("00000000-0000-0000-0000-000000000000").has_value());
}

TEST_CASE("McpUploadStaging::remove drops the ticket", "[McpUploadStaging]") {
  TempEnv env;
  vault::VaultRepository repo(env.vaultRoot());
  vault::McpUploadStaging staging(repo);
  const std::string id = staging.begin("notes/a.md", "x.bin");
  staging.remove(id);
  REQUIRE_FALSE(staging.get(id).has_value());
}
