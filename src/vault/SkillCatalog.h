#pragma once

#include "index/Database.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Turns `type: skill` documents into Agent Skills (agentskills.io) for the
// MCP skills extension (io.modelcontextprotocol/skills). A skill is the
// document's generated SKILL.md plus the files in its `<stem>.assets/`
// folder, served under `assets/`. Transport-independent: the MCP layers
// build skill:// URIs and JSON from these values.
namespace wikicore::vault {

// Agent Skills limits on `name`/`description`, and the extension's
// per-skill limits on files and bytes.
inline constexpr std::size_t kMaxSkillNameChars = 64;
inline constexpr std::size_t kMaxSkillDescriptionChars = 1024;
inline constexpr std::size_t kMaxSkillFiles = 512;
inline constexpr std::size_t kMaxSkillBytes = 16u * 1024u * 1024u;

inline constexpr std::string_view kSkillMarkdownFile = "SKILL.md";
inline constexpr std::string_view kSkillAssetsDir = "assets";

// The document's file basename without `.md`, lowercased, `_` turned into
// `-`, runs of `-` collapsed and leading/trailing `-` trimmed. nullopt when
// the result is not a valid Agent Skills name (empty, longer than 64, or
// any character outside [a-z0-9-], e.g. non-ASCII or a space).
std::optional<std::string> skillNameForPath(std::string_view documentPath);

// Markdown link destinations in `body` that point at a file of the
// document's own `.assets/` folder (any of the forms the app writes:
// `assets/<vault path>`, `<base>/assets/<vault path>`, percent-encoded or
// not) become `assets/<file>`, relative to the skill root. Other links are
// left untouched. `assetFiles` are the folder's file names.
std::string rewriteOwnAssetLinks(std::string_view body, std::string_view documentPath,
                                 const std::vector<std::string>& assetFiles);

// The SKILL.md bytes: a YAML block with exactly `name` and `description`
// (double-quoted, so a client parsing it gets back the same strings that
// are listed as the entry's frontmatter, whatever the text), then
// `# <title>` unless the body already starts with a level-1 heading, then
// the body.
std::string renderSkillMarkdown(std::string_view name, std::string_view description,
                                std::string_view title, std::string_view body);

// Percent-encodes everything outside [A-Za-z0-9._~-] in one path segment.
std::string percentEncodePathSegment(std::string_view segment);

struct SkillFile {
  std::string path;  // relative to the skill root, URI-encoded: "SKILL.md", "assets/x.png"
  std::string mimeType;
  std::string bytes;
  std::string digest;  // "sha256:<64 lowercase hex>" over `bytes`
};

struct Skill {
  std::string name;
  std::string description;
  std::string documentPath;
  bool isPublic = false;
  std::vector<SkillFile> files;  // SKILL.md first, then assets sorted by name
};

// A `type: skill` document that is not served, and why (shown to the
// admin; never sent to MCP clients).
struct SkippedSkill {
  std::string documentPath;
  std::string reason;
};

struct SkillListing {
  std::vector<Skill> skills;  // sorted by name
  std::vector<SkippedSkill> skipped;
};

class SkillCatalog {
 public:
  SkillCatalog(const index::Database& db, const DocumentService& documents,
               const AttachmentService& attachments, const VaultRepository& vault)
      : db_(db), documents_(documents), attachments_(attachments), vault_(vault) {}

  // Every servable skill visible at this scope. Each document's own
  // front-matter visibility is re-checked, not only the index's. Two
  // documents whose names collide are both skipped rather than one being
  // picked.
  SkillListing load(bool includePrivate) const;

  std::optional<Skill> find(std::string_view name, bool includePrivate) const;

 private:
  const index::Database& db_;
  const DocumentService& documents_;
  const AttachmentService& attachments_;
  const VaultRepository& vault_;
};

}  // namespace wikicore::vault
