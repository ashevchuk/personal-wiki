#include "vault/SkillCatalog.h"

#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "util/Sha256.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <catch2/catch_test_macros.hpp>
#include <yaml-cpp/yaml.h>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;
using namespace wikicore::vault;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-skill-catalog-test-" +
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
  VaultRepository repo{env.vaultRoot()};
  DocumentService documents{repo, indexUpdater, snapshots};
  AttachmentService attachments{repo};
  SkillCatalog catalog{db, documents, attachments, repo};

  Fixture() { db.migrate(); }

  void add(const std::string& path, const std::string& type, const std::string& description,
           const std::string& visibility = "public", const std::string& body = "Steps.\n",
           const std::string& title = "Title") {
    DocumentInput input;
    input.title = title;
    input.type = type;
    input.description = description;
    input.visibility = visibility;
    input.body = body;
    documents.create(path, input);
  }
};

std::vector<std::string> names(const SkillListing& listing) {
  std::vector<std::string> out;
  for (const auto& s : listing.skills) out.push_back(s.name);
  return out;
}

}  // namespace

TEST_CASE("skillNameForPath normalizes the file basename", "[SkillCatalog]") {
  REQUIRE(skillNameForPath("notes/gpg_sh.md") == std::string("gpg-sh"));
  REQUIRE(skillNameForPath("projects/ee/earth_meter.md") == std::string("earth-meter"));
  REQUIRE(skillNameForPath("notes/programming/llm/PUNK.md") == std::string("punk"));
  REQUIRE(skillNameForPath("a/__x__y--z_.MD") == std::string("x-y-z"));
  REQUIRE(skillNameForPath("a/review-2.md") == std::string("review-2"));
  REQUIRE_FALSE(skillNameForPath("a/my skill.md").has_value());
  REQUIRE_FALSE(skillNameForPath("a/\xd0\xbd\xd0\xbe\xd1\x82\xd0\xb0\xd1\x82\xd0\xba\xd0\xb0.md")
                    .has_value());
  REQUIRE_FALSE(skillNameForPath("a/___.md").has_value());
  REQUIRE_FALSE(skillNameForPath("a/" + std::string(65, 'x') + ".md").has_value());
  REQUIRE(skillNameForPath("a/" + std::string(64, 'x') + ".md") == std::string(64, 'x'));
}

TEST_CASE("renderSkillMarkdown: frontmatter parses back to exactly name + description",
          "[SkillCatalog]") {
  for (const std::string& description :
       {std::string("plain"), std::string("true"), std::string("null"), std::string("123"),
        std::string("Use when: a # b, [x] {y}"), std::string("- leading dash"),
        std::string("line1\nline2"), std::string("quotes \" and ' and \\ backslash"),
        std::string("\xd0\x9a\xd0\xbe\xd0\xbb\xd0\xb8 \xd1\x82\xd1\x80\xd0\xb5\xd0\xb1\xd0\xb0")}) {
    const std::string md = renderSkillMarkdown("deploy", description, "Deploy", "Body\n");
    REQUIRE(md.rfind("---\n", 0) == 0);
    const std::size_t end = md.find("\n---\n", 4);
    REQUIRE(end != std::string::npos);
    const YAML::Node fm = YAML::Load(md.substr(4, end - 4));
    REQUIRE(fm.IsMap());
    REQUIRE(fm.size() == 2);
    REQUIRE(fm["name"].as<std::string>() == "deploy");
    REQUIRE(fm["description"].as<std::string>() == description);
    // Double-quoted, so a YAML 1.2 parser cannot read "true"/"123" as non-strings.
    REQUIRE(fm["description"].Tag() == "!");
  }
}

TEST_CASE("renderSkillMarkdown adds the title heading only when the body has none",
          "[SkillCatalog]") {
  const std::string withHeading = renderSkillMarkdown("n", "d", "My Title", "Body\n");
  REQUIRE(withHeading.find("\n---\n\n# My Title\n\nBody\n") != std::string::npos);
  const std::string own = renderSkillMarkdown("n", "d", "My Title", "\n# Own\n\nBody\n");
  REQUIRE(own.find("# My Title") == std::string::npos);
  REQUIRE(own.find("# Own") != std::string::npos);
}

TEST_CASE("rewriteOwnAssetLinks maps every app link form to assets/<file>", "[SkillCatalog]") {
  const std::vector<std::string> files{"diagram.png", "my file.pdf"};
  const std::string body =
      "![d](/wiki/assets/notes/deploy.assets/diagram.png)\n"
      "![d](assets/notes/deploy.assets/diagram.png)\n"
      "[f](/assets/notes/deploy.assets/my%20file.pdf \"title\")\n"
      "[f](<https://h/wiki/assets/notes/deploy.assets/my file.pdf>)\n"
      "[other](/wiki/assets/notes/other.assets/diagram.png)\n"
      "[missing](/wiki/assets/notes/deploy.assets/gone.png)\n"
      "[web](https://example.com/x.png)\n"
      "[prefix](/wiki/xassets/notes/deploy.assets/diagram.png)\n";
  const std::string out = rewriteOwnAssetLinks(body, "notes/deploy.md", files);
  REQUIRE(out ==
          "![d](assets/diagram.png)\n"
          "![d](assets/diagram.png)\n"
          "[f](assets/my%20file.pdf \"title\")\n"
          "[f](<assets/my%20file.pdf>)\n"
          "[other](/wiki/assets/notes/other.assets/diagram.png)\n"
          "[missing](/wiki/assets/notes/deploy.assets/gone.png)\n"
          "[web](https://example.com/x.png)\n"
          "[prefix](/wiki/xassets/notes/deploy.assets/diagram.png)\n");
  REQUIRE(rewriteOwnAssetLinks(body, "notes/deploy.md", {}) == body);
}

TEST_CASE("SkillCatalog serves type: skill documents with SKILL.md and assets",
          "[SkillCatalog]") {
  Fixture f;
  f.add("notes/deploy_steps.md", "Skill", "Use when deploying",
        "public", "Run it.\n![d](/wiki/assets/notes/deploy_steps.assets/diagram.png)\n",
        "Deploy Steps");
  f.attachments.store("notes/deploy_steps.md", "diagram.png", std::string("\x89PNG\0x", 6));
  f.add("notes/plain.md", "note", "not a skill");
  f.add("notes/skillful.md", "skills", "type contains the word but is not skill");

  const SkillListing listing = f.catalog.load(true);
  REQUIRE(names(listing) == std::vector<std::string>{"deploy-steps"});
  REQUIRE(listing.skipped.empty());
  const Skill& skill = listing.skills.front();
  REQUIRE(skill.description == "Use when deploying");
  REQUIRE(skill.documentPath == "notes/deploy_steps.md");
  REQUIRE(skill.isPublic);
  REQUIRE(skill.files.size() == 2);

  const SkillFile& md = skill.files[0];
  REQUIRE(md.path == "SKILL.md");
  REQUIRE(md.mimeType == "text/markdown");
  REQUIRE(md.bytes.find("name: \"deploy-steps\"") != std::string::npos);
  REQUIRE(md.bytes.find("# Deploy Steps") != std::string::npos);
  REQUIRE(md.bytes.find("![d](assets/diagram.png)") != std::string::npos);
  REQUIRE(md.digest == "sha256:" + util::sha256Hex(md.bytes));

  const SkillFile& asset = skill.files[1];
  REQUIRE(asset.path == "assets/diagram.png");
  REQUIRE(asset.mimeType == "image/png");
  REQUIRE(asset.bytes == std::string("\x89PNG\0x", 6));
  REQUIRE(asset.digest == "sha256:" + util::sha256Hex(asset.bytes));

  REQUIRE(f.catalog.find("deploy-steps", true).has_value());
  REQUIRE_FALSE(f.catalog.find("plain", true).has_value());
}

TEST_CASE("SkillCatalog hides private skills from the public scope", "[SkillCatalog]") {
  Fixture f;
  f.add("a/open.md", "skill", "public one", "public");
  f.add("a/secret.md", "skill", "private one", "private");
  REQUIRE(names(f.catalog.load(true)) == std::vector<std::string>{"open", "secret"});
  REQUIRE(names(f.catalog.load(false)) == std::vector<std::string>{"open"});
  REQUIRE(f.catalog.load(false).skipped.empty());
  REQUIRE_FALSE(f.catalog.find("secret", false).has_value());
  REQUIRE_FALSE(f.catalog.load(true).skills[1].isPublic);
}

TEST_CASE("SkillCatalog skips invalid skills with a reason", "[SkillCatalog]") {
  Fixture f;
  f.add("a/no-description.md", "skill", "");
  f.add("a/blank.md", "skill", "   ");
  f.add("a/long.md", "skill", std::string(1025, 'x'));
  f.add("a/max.md", "skill", std::string(1024, 'x'));
  f.add("a/bad name.md", "skill", "fine");
  f.add("x/dup_name.md", "skill", "first");
  f.add("y/dup-name.md", "skill", "second");

  const SkillListing listing = f.catalog.load(true);
  REQUIRE(names(listing) == std::vector<std::string>{"max"});
  std::map<std::string, std::string> reasons;
  for (const auto& s : listing.skipped) reasons[s.documentPath] = s.reason;
  REQUIRE(reasons.size() == 6);
  REQUIRE(reasons["a/no-description.md"] == "missing description");
  REQUIRE(reasons["a/blank.md"] == "missing description");
  REQUIRE(reasons["a/long.md"] == "description is longer than 1024 characters");
  REQUIRE(reasons["a/bad name.md"].find("valid skill name") != std::string::npos);
  REQUIRE(reasons["x/dup_name.md"].find("more than one document") != std::string::npos);
  REQUIRE(reasons["y/dup-name.md"].find("more than one document") != std::string::npos);
}

TEST_CASE("SkillCatalog: a name collision with a private document still blocks the public "
          "one, without revealing it",
          "[SkillCatalog]") {
  Fixture f;
  f.add("x/shared.md", "skill", "public", "public");
  f.add("y/shared.md", "skill", "private", "private");
  const SkillListing listing = f.catalog.load(false);
  REQUIRE(listing.skills.empty());
  REQUIRE(listing.skipped.size() == 1);
  REQUIRE(listing.skipped[0].documentPath == "x/shared.md");
  REQUIRE(listing.skipped[0].reason.find("y/") == std::string::npos);
}

TEST_CASE("SkillCatalog counts description length in characters, not bytes",
          "[SkillCatalog]") {
  Fixture f;
  std::string cyrillic;
  for (int i = 0; i < 1024; ++i) cyrillic += "\xd1\x8f";  // 2048 bytes, 1024 characters
  f.add("a/ua.md", "skill", cyrillic);
  REQUIRE(names(f.catalog.load(true)) == std::vector<std::string>{"ua"});
}
