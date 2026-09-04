#include "vault/BackupService.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wikicore::vault;

namespace {

// RAII temp vault dir, same shape as every other service test in this
// suite (see FolderServiceTest.cpp's TempEnv).
class TempVault {
 public:
  TempVault()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-backupsvc-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "vault" / "notes");
    fs::create_directories(root_ / "vault" / ".trash");
    fs::create_directories(root_ / "vault" / ".uploads-tmp" / "tmp" / "AB");
    write(root_ / "vault" / "welcome.md", "# Welcome\n");
    write(root_ / "vault" / "notes" / "sub.md", "# Sub\n");
    write(root_ / "vault" / ".trash" / "deleted.md", "# Gone\n");
    write(root_ / "vault" / ".uploads-tmp" / "tmp" / "AB" / "fragment", "junk");
  }
  ~TempVault() { fs::remove_all(root_); }

  TempVault(const TempVault&) = delete;
  TempVault& operator=(const TempVault&) = delete;

  fs::path vaultPath() const { return root_ / "vault"; }

 private:
  static void write(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    f << content;
  }

  fs::path root_;
};

// Lists a .tar.gz's member paths by shelling out to `tar tzf` on a temp
// file holding the archive bytes -- the same trusted tool BackupService
// itself uses to build the archive, just run in reverse to check the
// test's own assumptions about what's inside without hand-rolling a
// gzip/tar reader for a handful of assertions.
std::vector<std::string> listArchiveMembers(const std::vector<char>& archive) {
  const fs::path tmpFile =
      fs::temp_directory_path() / fs::path("wiki-backupsvc-test-archive.tar.gz");
  {
    std::ofstream f(tmpFile, std::ios::binary);
    f.write(archive.data(), static_cast<std::streamsize>(archive.size()));
  }

  std::vector<std::string> members;
  const std::string cmd = "tar tzf " + tmpFile.string();
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    std::array<char, 512> line{};
    while (fgets(line.data(), line.size(), pipe)) {
      std::string entry(line.data());
      if (!entry.empty() && entry.back() == '\n') entry.pop_back();
      members.push_back(entry);
    }
    pclose(pipe);
  }
  fs::remove(tmpFile);
  return members;
}

bool contains(const std::vector<std::string>& members, const std::string& needle) {
  for (const auto& m : members) {
    if (m == needle || m.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("createVaultBackup: succeeds and includes documents, .trash/",
          "[BackupService]") {
  TempVault env;
  const BackupResult result = createVaultBackup(env.vaultPath().string());

  REQUIRE(result.success);
  REQUIRE(result.errorMessage.empty());
  REQUIRE_FALSE(result.archive.empty());

  const auto members = listArchiveMembers(result.archive);
  REQUIRE(contains(members, "welcome.md"));
  REQUIRE(contains(members, "notes/sub.md"));
  REQUIRE(contains(members, ".trash/deleted.md"));
}

TEST_CASE("createVaultBackup: excludes .uploads-tmp/", "[BackupService]") {
  TempVault env;
  const BackupResult result = createVaultBackup(env.vaultPath().string());

  REQUIRE(result.success);
  const auto members = listArchiveMembers(result.archive);
  for (const auto& m : members) {
    INFO("archive member: " << m);
    REQUIRE(m.find(".uploads-tmp") == std::string::npos);
  }
}

TEST_CASE("createVaultBackup: a non-existent path fails cleanly instead of producing "
          "an empty/garbage archive",
          "[BackupService]") {
  const BackupResult result =
      createVaultBackup("/this/path/does/not/exist/wiki-backupsvc-test");

  REQUIRE_FALSE(result.success);
  REQUIRE_FALSE(result.errorMessage.empty());
  REQUIRE(result.archive.empty());
}
