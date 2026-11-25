#include "core/McpSkills.h"

#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "util/Base64.h"
#include "util/Sha256.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/SkillCatalog.h"
#include "vault/VaultRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;
using json = nlohmann::ordered_json;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-mcp-skills-test-" +
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

struct Fixture {
  TempEnv env;
  index::Database db{env.dbPath()};
  index::IndexUpdater indexUpdater{db};
  index::SnapshotStore snapshots{db};
  vault::VaultRepository repo{env.vaultRoot()};
  vault::DocumentService documents{repo, indexUpdater, snapshots};
  vault::AttachmentService attachments{repo};
  vault::SkillCatalog catalog{db, documents, attachments, repo};

  Fixture() {
    db.migrate();
    add("skills/deploy.md", "Use when deploying", "public");
    attachments.store("skills/deploy.md", "logo.png", std::string("\x89PNG\0\x01", 6));
    attachments.store("skills/deploy.md", "notes.txt", "plain text\n");
    add("skills/secret.md", "Private playbook", "private");
  }

  void add(const std::string& path, const std::string& description, const std::string& visibility) {
    vault::DocumentInput input;
    input.title = "T";
    input.type = "skill";
    input.description = description;
    input.visibility = visibility;
    input.body = "Body\n";
    documents.create(path, input);
  }

  mcp::MethodOutcome call(const std::string& method, json params, bool includePrivate = true) {
    return mcp::handleSkillsMethod(method, params, catalog, includePrivate);
  }
};

}  // namespace

TEST_CASE("modernServerCapabilities declares resources and the skills extension",
          "[McpSkills]") {
  const json caps = mcp::modernServerCapabilities();
  REQUIRE(caps.contains("tools"));
  REQUIRE(caps["resources"].is_object());
  REQUIRE(caps["extensions"]["io.modelcontextprotocol/skills"] == json::object());
}

TEST_CASE("skills/list returns complete entries with digests over the served bytes",
          "[McpSkills]") {
  Fixture f;
  const auto out = f.call("skills/list", json::object(), false);
  REQUIRE_FALSE(out.error);
  const json& r = out.result;
  REQUIRE(r["resultType"] == "complete");
  REQUIRE(r["ttlMs"] == mcp::kSkillsTtlMs);
  REQUIRE(r["cacheScope"] == "public");
  REQUIRE(r["skills"].size() == 1);
  const json& entry = r["skills"][0];
  REQUIRE(entry["uri"] == "skill://deploy/SKILL.md");
  REQUIRE(entry["frontmatter"] == json{{"name", "deploy"}, {"description", "Use when deploying"}});
  REQUIRE(entry["resources"].size() == 3);
  REQUIRE(entry["resources"][0]["uri"] == "skill://deploy/SKILL.md");

  for (const auto& res : entry["resources"]) {
    const auto read = f.call("resources/read", json{{"uri", res["uri"]}}, false);
    REQUIRE_FALSE(read.error);
    const json& c = read.result["contents"][0];
    const std::string bytes = c.contains("text") ? c["text"].get<std::string>()
                                                 : *util::decodeBase64(c["blob"].get<std::string>());
    REQUIRE(res["size"] == bytes.size());
    REQUIRE(res["digest"] == "sha256:" + util::sha256Hex(bytes));
  }
}

TEST_CASE("skills/list is private-scoped when it includes a private skill", "[McpSkills]") {
  Fixture f;
  const auto out = f.call("skills/list", json::object(), true);
  REQUIRE(out.result["skills"].size() == 2);
  REQUIRE(out.result["cacheScope"] == "private");
}

TEST_CASE("skills/get answers by SKILL.md URI only", "[McpSkills]") {
  Fixture f;
  const auto ok = f.call("skills/get", json{{"uri", "skill://deploy/SKILL.md"}});
  REQUIRE_FALSE(ok.error);
  REQUIRE(ok.result["skill"]["frontmatter"]["name"] == "deploy");
  REQUIRE(ok.result["cacheScope"] == "public");
  REQUIRE(f.call("skills/get", json{{"uri", "skill://secret/SKILL.md"}}).result["cacheScope"] ==
          "private");

  for (const char* uri : {"skill://deploy/assets/logo.png", "skill://nope/SKILL.md",
                          "skill://deploy", "file:///etc/passwd"}) {
    const auto err = f.call("skills/get", json{{"uri", uri}});
    REQUIRE(err.error);
    REQUIRE(err.error->code == mcp::kErrorInvalidParams);
  }
  REQUIRE(f.call("skills/get", json::object()).error->code == mcp::kErrorInvalidParams);
  REQUIRE(f.call("skills/get", json{{"uri", "skill://secret/SKILL.md"}}, false).error);
}

TEST_CASE("resources/read: SKILL.md and text as text, binary as blob, unknown -32602",
          "[McpSkills]") {
  Fixture f;
  const auto md = f.call("resources/read", json{{"uri", "skill://deploy/SKILL.md"}});
  REQUIRE(md.result["contents"][0]["mimeType"] == "text/markdown");
  REQUIRE(md.result["contents"][0]["text"].get<std::string>().rfind("---\n", 0) == 0);

  const auto txt = f.call("resources/read", json{{"uri", "skill://deploy/assets/notes.txt"}});
  REQUIRE(txt.result["contents"][0]["text"] == "plain text\n");

  const auto png = f.call("resources/read", json{{"uri", "skill://deploy/assets/logo.png"}});
  REQUIRE(png.result["contents"][0].contains("blob"));
  REQUIRE(*util::decodeBase64(png.result["contents"][0]["blob"].get<std::string>()) ==
          std::string("\x89PNG\0\x01", 6));

  for (const char* uri :
       {"skill://deploy/assets/missing.png", "skill://deploy/../secret/SKILL.md",
        "skill://deploy/assets/../../skills/deploy.md", "skill://nope/SKILL.md", "skill://"}) {
    const auto err = f.call("resources/read", json{{"uri", uri}});
    REQUIRE(err.error);
    REQUIRE(err.error->code == mcp::kErrorInvalidParams);
  }
  REQUIRE(f.call("resources/read", json{{"uri", "skill://secret/SKILL.md"}}, false).error);
}

TEST_CASE("resources/list lists every skill file; cursors are rejected", "[McpSkills]") {
  Fixture f;
  const auto out = f.call("resources/list", json::object(), false);
  REQUIRE_FALSE(out.error);
  REQUIRE(out.result["resources"].size() == 3);
  REQUIRE(out.result["resources"][0]["name"] == "deploy");
  REQUIRE(out.result["resources"][0]["description"] == "Use when deploying");
  REQUIRE(out.result["resources"][1]["name"] == "logo.png");
  REQUIRE(out.result["cacheScope"] == "public");
  REQUIRE(f.call("resources/list", json{{"cursor", "x"}}).error->code == mcp::kErrorInvalidParams);
  REQUIRE(f.call("skills/list", json{{"cursor", "x"}}).error->code == mcp::kErrorInvalidParams);
}

TEST_CASE("isValidUtf8", "[McpSkills]") {
  REQUIRE(mcp::isValidUtf8("plain"));
  REQUIRE(mcp::isValidUtf8("\xd1\x8f \xe4\xb8\x96 \xf0\x9f\x98\x80"));
  REQUIRE_FALSE(mcp::isValidUtf8("\xff"));
  REQUIRE_FALSE(mcp::isValidUtf8("\xc0\xaf"));          // overlong '/'
  REQUIRE_FALSE(mcp::isValidUtf8("\xed\xa0\x80"));      // surrogate
  REQUIRE_FALSE(mcp::isValidUtf8("\xf4\x90\x80\x80"));  // above U+10FFFF
  REQUIRE_FALSE(mcp::isValidUtf8("\xe4\xb8"));          // truncated
}
