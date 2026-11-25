#include "vault/SkillCatalog.h"

#include "index/Statement.h"
#include "util/Sha256.h"
#include "vault/SkillDocuments.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

constexpr std::string_view kMarkdownMimeType = "text/markdown";

bool isAsciiSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view trimAscii(std::string_view s) {
  while (!s.empty() && isAsciiSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isAsciiSpace(s.back())) s.remove_suffix(1);
  return s;
}

std::size_t utf8CodePoints(std::string_view s) {
  return static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  }));
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Invalid escapes stay literal; this only feeds a comparison.
std::string percentDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexValue(s[i + 1]);
      const int lo = hexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// "notes/foo.md" -> "notes/foo.assets", the same layout AttachmentService
// stores attachments in.
std::string assetsDirOf(std::string_view documentPath) {
  const fs::path doc{std::string(documentPath)};
  return (doc.parent_path() / (doc.stem().string() + ".assets")).generic_string();
}

// The document's own assets folder as every link form the app writes or
// keeps up to date on rename (DocumentService::rewriteAssetHrefs).
struct OwnAssetPrefixes {
  std::string sameFolder;    // "foo.assets/"
  std::string vaultPath;     // "notes/foo.assets/"
  std::string baseRelative;  // "assets/notes/foo.assets/"
  std::string pathAbsolute;  // "/assets/notes/foo.assets/", after any base path
};

OwnAssetPrefixes ownAssetPrefixes(std::string_view documentPath) {
  const std::string dir = assetsDirOf(documentPath);
  return {fs::path{std::string(documentPath)}.stem().string() + ".assets/", dir + "/",
          "assets/" + dir + "/", "/assets/" + dir + "/"};
}

bool startsWith(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

// The asset file a link destination points at, when it is one of
// `assetFiles` in the document's own assets folder. Destinations with a
// scheme or a host (`https://...`, `//host/...`) point elsewhere, even when
// their path looks the same.
std::optional<std::string> ownAssetTarget(std::string_view destination,
                                          const OwnAssetPrefixes& prefixes,
                                          const std::set<std::string>& assetFiles) {
  const std::string decoded = percentDecode(destination.substr(0, destination.find_first_of("?#")));
  if (decoded.find("://") != std::string::npos || startsWith(decoded, "//")) return std::nullopt;

  std::optional<std::string> file;
  for (const std::string* prefix :
       {&prefixes.sameFolder, &prefixes.vaultPath, &prefixes.baseRelative}) {
    if (startsWith(decoded, *prefix)) file = decoded.substr(prefix->size());
  }
  if (!file && startsWith(decoded, "/")) {
    const std::size_t pos = decoded.rfind(prefixes.pathAbsolute);
    if (pos != std::string::npos) file = decoded.substr(pos + prefixes.pathAbsolute.size());
  }
  if (file && assetFiles.count(*file) != 0) return file;
  return std::nullopt;
}

bool startsWithLevelOneHeading(std::string_view body) {
  while (!body.empty() && isAsciiSpace(body.front())) body.remove_prefix(1);
  return body.size() >= 2 && body[0] == '#' && body[1] == ' ';
}

SkillFile makeFile(std::string path, std::string mimeType, std::string bytes) {
  SkillFile file;
  file.path = std::move(path);
  file.mimeType = std::move(mimeType);
  file.digest = "sha256:" + util::sha256Hex(bytes);
  file.bytes = std::move(bytes);
  return file;
}

struct Candidate {
  std::string path;
  DocumentRecord record;
  std::optional<std::string> name;
};

}  // namespace

std::optional<std::string> skillNameForPath(std::string_view documentPath) {
  std::string base = fs::path{std::string(documentPath)}.filename().string();
  if (base.size() >= 3) {
    std::string ext = base.substr(base.size() - 3);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".md") base.resize(base.size() - 3);
  }

  std::string name;
  for (const char raw : base) {
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    if (c == '_') c = '-';
    if (c == '-' && (name.empty() || name.back() == '-')) continue;
    name += c;
  }
  while (!name.empty() && name.back() == '-') name.pop_back();

  const bool validChars = std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
  if (name.empty() || name.size() > kMaxSkillNameChars || !validChars) return std::nullopt;
  return name;
}

std::string percentEncodePathSegment(std::string_view segment) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : segment) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '.' || c == '_' || c == '~' || c == '-') {
      out += c;
    } else {
      out += '%';
      out += kHex[u >> 4];
      out += kHex[u & 0xF];
    }
  }
  return out;
}

std::string rewriteOwnAssetLinks(std::string_view body, std::string_view documentPath,
                                 const std::vector<std::string>& assetFiles) {
  if (assetFiles.empty()) return std::string(body);
  const std::set<std::string> files(assetFiles.begin(), assetFiles.end());
  const OwnAssetPrefixes prefixes = ownAssetPrefixes(documentPath);

  std::string out;
  out.reserve(body.size());
  std::size_t copied = 0;
  std::size_t open = body.find("](");
  while (open != std::string_view::npos) {
    std::size_t start = open + 2;
    std::size_t end;
    if (start < body.size() && body[start] == '<') {
      ++start;
      end = body.find('>', start);
    } else {
      end = start;
      while (end < body.size() && body[end] != ')' && !isAsciiSpace(body[end])) ++end;
    }
    if (end == std::string_view::npos) break;
    const auto target = ownAssetTarget(body.substr(start, end - start), prefixes, files);
    if (target) {
      out.append(body.substr(copied, start - copied));
      out += std::string(kSkillAssetsDir) + "/" + percentEncodePathSegment(*target);
      copied = end;
    }
    // Resume after this destination, so a "](" inside it is never treated
    // as another link and `copied` only moves forward.
    open = body.find("](", end);
  }
  out.append(body.substr(copied));
  return out;
}

std::string renderSkillMarkdown(std::string_view name, std::string_view description,
                                std::string_view title, std::string_view body) {
  YAML::Emitter yaml;
  yaml << YAML::BeginMap;
  yaml << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << std::string(name);
  yaml << YAML::Key << "description" << YAML::Value << YAML::DoubleQuoted
       << std::string(description);
  yaml << YAML::EndMap;

  std::string out = "---\n" + std::string(yaml.c_str()) + "\n---\n\n";
  if (!title.empty() && !startsWithLevelOneHeading(body)) {
    out += "# " + std::string(title) + "\n\n";
  }
  out += body;
  return out;
}

namespace {

// Every type: skill document, with its derived name. Reads each document's
// markdown (small), never its attachments.
std::vector<Candidate> skillCandidates(const index::Database& db,
                                       const DocumentService& documents) {
  // The LIKE only narrows the scan; isSkillType decides, so the catalog and
  // the write guard (SkillDocuments.h) agree on what a skill is.
  index::Statement stmt(db.handle(),
                        "SELECT path FROM documents WHERE doc_type LIKE '%skill%' ORDER BY path;");
  std::vector<Candidate> candidates;
  while (stmt.step()) {
    const std::string path = stmt.columnText(0);
    try {
      DocumentRecord record = documents.get(path);
      if (!isSkillType(record.frontMatter.type)) continue;
      candidates.push_back(Candidate{path, std::move(record), skillNameForPath(path)});
    } catch (const std::exception&) {
      // Index ahead of the vault (file just removed); the next rescan fixes it.
    }
  }
  return candidates;
}

std::map<std::string, int> countNames(const std::vector<Candidate>& candidates) {
  std::map<std::string, int> uses;
  for (const auto& c : candidates) {
    if (c.name) ++uses[*c.name];
  }
  return uses;
}

bool visibleAt(const Candidate& c, bool includePrivate) {
  return includePrivate || c.record.frontMatter.visibility == "public";
}

struct BuildResult {
  std::optional<Skill> skill;
  std::string skipReason;  // set when `skill` is not
};

// Validates one candidate and reads its attachments.
BuildResult buildSkill(const Candidate& c, const std::map<std::string, int>& nameUses,
                       const AttachmentService& attachments, const VaultRepository& vault) {
  const FrontMatter& fm = c.record.frontMatter;
  if (!c.name) {
    return {std::nullopt,
            "file name does not give a valid skill name (a-z, 0-9 and '-', at most 64)"};
  }
  if (nameUses.at(*c.name) > 1) {
    return {std::nullopt, "skill name '" + *c.name + "' is used by more than one document"};
  }
  const std::string_view description = trimAscii(fm.description);
  if (description.empty()) return {std::nullopt, "missing description"};
  if (utf8CodePoints(description) > kMaxSkillDescriptionChars) {
    return {std::nullopt, "description is longer than 1024 characters"};
  }

  std::vector<AttachmentInfo> assets;
  try {
    assets = attachments.listForDocument(c.path);
  } catch (const std::exception&) {
    return {std::nullopt, "its attachments could not be listed"};
  }
  if (assets.size() + 1 > kMaxSkillFiles) {
    return {std::nullopt, "more than 512 files including attachments"};
  }
  std::vector<std::string> assetNames;
  for (const auto& a : assets) assetNames.push_back(fs::path(a.relativePath).filename().string());

  Skill skill;
  skill.name = *c.name;
  skill.description = std::string(description);
  skill.documentPath = c.path;
  skill.isPublic = fm.visibility == "public";
  skill.files.push_back(makeFile(
      std::string(kSkillMarkdownFile), std::string(kMarkdownMimeType),
      renderSkillMarkdown(skill.name, skill.description, fm.title,
                          rewriteOwnAssetLinks(c.record.body, c.path, assetNames))));

  std::size_t totalBytes = skill.files.front().bytes.size();
  for (std::size_t i = 0; i < assets.size() && totalBytes <= kMaxSkillBytes; ++i) {
    std::string bytes;
    try {
      bytes = vault.readRaw(assets[i].relativePath);
    } catch (const std::exception&) {
      return {std::nullopt, "an attachment could not be read"};
    }
    totalBytes += bytes.size();
    skill.files.push_back(makeFile(
        std::string(kSkillAssetsDir) + "/" + percentEncodePathSegment(assetNames[i]),
        assets[i].mimeType, std::move(bytes)));
  }
  if (totalBytes > kMaxSkillBytes) return {std::nullopt, "more than 16 MiB including attachments"};
  return {std::move(skill), {}};
}

}  // namespace

SkillListing SkillCatalog::load(bool includePrivate) const {
  const std::vector<Candidate> candidates = skillCandidates(db_, documents_);
  const std::map<std::string, int> nameUses = countNames(candidates);

  SkillListing listing;
  for (const auto& c : candidates) {
    if (!visibleAt(c, includePrivate)) continue;
    BuildResult built = buildSkill(c, nameUses, attachments_, vault_);
    if (built.skill) {
      listing.skills.push_back(std::move(*built.skill));
    } else {
      listing.skipped.push_back(SkippedSkill{c.path, std::move(built.skipReason)});
    }
  }
  std::sort(listing.skills.begin(), listing.skills.end(),
            [](const Skill& a, const Skill& b) { return a.name < b.name; });
  return listing;
}

// Builds only the named skill: attachments of other skills are never read.
std::optional<Skill> SkillCatalog::find(std::string_view name, bool includePrivate) const {
  const std::vector<Candidate> candidates = skillCandidates(db_, documents_);
  const std::map<std::string, int> nameUses = countNames(candidates);
  for (const auto& c : candidates) {
    if (c.name != name || !visibleAt(c, includePrivate)) continue;
    return buildSkill(c, nameUses, attachments_, vault_).skill;
  }
  return std::nullopt;
}

}  // namespace wikicore::vault
