#include "vault/FrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace wikicore::vault {

namespace {

// Locates a leading "---" delimiter line and the "---" (or "...") line
// that closes it. Returns {yamlBlock, bodyStart} on success, nullopt if
// `content` doesn't open with a front-matter block at all.
std::optional<std::pair<std::string, size_t>> splitFrontMatterBlock(
    std::string_view content) {
  auto stripCr = [](std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
  };

  const size_t firstLineEnd = content.find('\n');
  const std::string_view firstLine = (firstLineEnd == std::string_view::npos)
                                          ? content
                                          : content.substr(0, firstLineEnd);
  if (stripCr(firstLine) != "---" || firstLineEnd == std::string_view::npos) {
    return std::nullopt;  // no opening delimiter, or no room for a body
  }

  const size_t yamlStart = firstLineEnd + 1;
  size_t searchPos = yamlStart;
  while (searchPos <= content.size()) {
    const size_t lineEnd = content.find('\n', searchPos);
    const std::string_view line = (lineEnd == std::string_view::npos)
                                       ? content.substr(searchPos)
                                       : content.substr(searchPos, lineEnd - searchPos);
    if (const std::string_view trimmed = stripCr(line);
        trimmed == "---" || trimmed == "...") {
      std::string yamlBlock(content.substr(yamlStart, searchPos - yamlStart));
      const size_t bodyStart =
          (lineEnd == std::string_view::npos) ? content.size() : lineEnd + 1;
      return std::make_pair(std::move(yamlBlock), bodyStart);
    }
    if (lineEnd == std::string_view::npos) break;  // EOF, no closer found
    searchPos = lineEnd + 1;
  }
  return std::nullopt;
}

std::vector<std::string> readTags(const YAML::Node& node) {
  std::vector<std::string> tags;
  if (!node.IsSequence()) return tags;
  for (const auto& item : node) {
    if (item.IsScalar()) {
      tags.push_back(item.as<std::string>());
    }
  }
  return tags;
}

std::string readScalarOr(const YAML::Node& node, const std::string& fallback) {
  if (!node.IsDefined() || node.IsNull() || !node.IsScalar()) return fallback;
  return node.as<std::string>();
}

std::optional<std::string> readScalarIfPresent(const YAML::Node& node) {
  if (!node.IsDefined() || node.IsMap() || node.IsSequence()) return std::nullopt;
  if (node.IsNull()) return std::string();
  return node.as<std::string>();
}

}  // namespace

ParsedDocument parseFrontMatter(std::string_view rawContent) {
  ParsedDocument result;

  const auto split = splitFrontMatterBlock(rawContent);
  if (!split) {
    result.body = std::string(rawContent);
    return result;  // fail-safe defaults, whole input is the body
  }
  const auto& [yamlBlock, bodyStart] = *split;
  result.body = std::string(rawContent.substr(bodyStart));

  YAML::Node doc;
  try {
    doc = YAML::Load(yamlBlock);
  } catch (const YAML::Exception&) {
    // Malformed YAML: treat as if there were no front matter at all,
    // rather than half-trusting a block we couldn't actually parse.
    result.body = std::string(rawContent);
    return result;
  }
  if (!doc.IsMap()) {
    result.body = std::string(rawContent);
    return result;
  }

  FrontMatter fm;
  fm.id = readScalarOr(doc["id"], "");
  fm.title = readScalarOr(doc["title"], "");
  fm.tags = readTags(doc["tags"]);
  fm.type = readScalarOr(doc["type"], "");
  fm.description = readScalarOr(doc["description"], "");
  fm.created = readScalarOr(doc["created"], "");
  fm.updated = readScalarOr(doc["updated"], "");
  fm.due = readScalarOr(doc["due"], "");
  fm.recur = readScalarOr(doc["recur"], "");

  // The one field where "parsed successfully but not what we expect"
  // must still collapse to the safe default, not pass through verbatim.
  const std::string rawVisibility = readScalarOr(doc["visibility"], "private");
  fm.visibility = (rawVisibility == "public") ? "public" : "private";

  result.frontMatter = std::move(fm);
  return result;
}

ImportedDocument parseForImport(std::string_view rawContent) {
  ImportedDocument result;
  result.body = std::string(rawContent);

  const auto split = splitFrontMatterBlock(rawContent);
  if (!split) return result;

  YAML::Node doc;
  try {
    doc = YAML::Load(split->first);
  } catch (const YAML::Exception&) {
    return result;
  }
  if (!doc.IsMap()) return result;

  result.body = std::string(rawContent.substr(split->second));
  ImportedFields& fields = result.fields;
  fields.title = readScalarIfPresent(doc["title"]);
  fields.type = readScalarIfPresent(doc["type"]);
  fields.due = readScalarIfPresent(doc["due"]);
  fields.recur = readScalarIfPresent(doc["recur"]);
  if (const auto visibility = readScalarIfPresent(doc["visibility"])) {
    fields.visibility = (*visibility == "public") ? "public" : "private";
  }
  const YAML::Node tags = doc["tags"];
  if (tags.IsSequence()) {
    fields.tags = readTags(tags);
  } else if (tags.IsDefined() && tags.IsNull()) {
    fields.tags = std::vector<std::string>{};
  }
  return result;
}

namespace {

constexpr std::array<std::string_view, 10> kModeledKeys = {
    "id",      "title",   "tags", "visibility", "type", "description",
    "created", "updated", "due",  "recur"};

// Top-level entries of `existingRaw`'s front matter whose key FrontMatter
// does not model, in file order. Empty when there is no usable block.
std::vector<std::pair<YAML::Node, YAML::Node>> unmodeledEntries(std::string_view existingRaw) {
  std::vector<std::pair<YAML::Node, YAML::Node>> entries;
  const auto split = splitFrontMatterBlock(existingRaw);
  if (!split) return entries;
  YAML::Node doc;
  try {
    doc = YAML::Load(split->first);
  } catch (const YAML::Exception&) {
    return entries;
  }
  if (!doc.IsMap()) return entries;
  for (const auto& entry : doc) {
    const bool modeled =
        entry.first.IsScalar() &&
        std::find(kModeledKeys.begin(), kModeledKeys.end(), entry.first.Scalar()) !=
            kModeledKeys.end();
    if (!modeled) entries.emplace_back(entry.first, entry.second);
  }
  return entries;
}

struct IdLines {
  int count = 0;
  size_t valueStart = 0;  // first byte after "id:" of the last match
  size_t valueEnd = 0;    // end of that line, excluding "\r\n"/"\n"
};

// Top-level `id:` lines inside the block that spans [yamlStart, yamlEnd).
IdLines findTopLevelIdLines(std::string_view raw, size_t yamlStart, size_t yamlEnd) {
  constexpr std::string_view kIdKey = "id:";
  IdLines found;
  for (size_t lineStart = yamlStart; lineStart < yamlEnd;) {
    size_t lineEnd = raw.find('\n', lineStart);
    if (lineEnd == std::string_view::npos || lineEnd > yamlEnd) lineEnd = yamlEnd;
    std::string_view line = raw.substr(lineStart, lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    const bool isTopLevelIdKey =
        line.substr(0, kIdKey.size()) == kIdKey &&
        (line.size() == kIdKey.size() || line[kIdKey.size()] == ' ' ||
         line[kIdKey.size()] == '\t');
    if (isTopLevelIdKey) {
      ++found.count;
      found.valueStart = lineStart + kIdKey.size();
      found.valueEnd = lineStart + line.size();
    }
    lineStart = lineEnd + 1;
  }
  return found;
}

bool differsOnlyInId(std::string_view original, std::string_view rewritten,
                     std::string_view newId) {
  const ParsedDocument before = parseFrontMatter(original);
  const ParsedDocument after = parseFrontMatter(rewritten);
  const FrontMatter& b = before.frontMatter;
  const FrontMatter& a = after.frontMatter;
  return a.id == newId && after.body == before.body && a.title == b.title &&
         a.tags == b.tags && a.visibility == b.visibility && a.type == b.type &&
         a.description == b.description && a.created == b.created && a.updated == b.updated && a.due == b.due &&
         a.recur == b.recur;
}

}  // namespace

std::string serializeFrontMatter(const FrontMatter& fm, const std::string& body) {
  return serializeFrontMatter(fm, body, {});
}

std::string serializeFrontMatter(const FrontMatter& fm, const std::string& body,
                                 std::string_view existingRaw) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "id" << YAML::Value << fm.id;
  out << YAML::Key << "title" << YAML::Value << fm.title;
  out << YAML::Key << "tags" << YAML::Value << YAML::Flow << YAML::BeginSeq;
  for (const auto& tag : fm.tags) out << tag;
  out << YAML::EndSeq;
  // visibility is re-validated here too (not just on parse) so a caller
  // can never write a FrontMatter with visibility left at some other
  // in-memory value than exactly "public"/"private" to disk.
  out << YAML::Key << "visibility" << YAML::Value
      << (fm.visibility == "public" ? "public" : "private");
  out << YAML::Key << "type" << YAML::Value << fm.type;
  if (!fm.description.empty()) {
    out << YAML::Key << "description" << YAML::Value << fm.description;
  }
  out << YAML::Key << "created" << YAML::Value << fm.created;
  out << YAML::Key << "updated" << YAML::Value << fm.updated;
  out << YAML::Key << "due" << YAML::Value << fm.due;
  out << YAML::Key << "recur" << YAML::Value << fm.recur;
  for (const auto& [key, value] : unmodeledEntries(existingRaw)) {
    out << YAML::Key << key << YAML::Value << value;
  }
  out << YAML::EndMap;

  return "---\n" + std::string(out.c_str()) + "\n---\n" + body;
}

std::string replaceBody(std::string_view rawContent, std::string_view newBody) {
  const size_t bodySize = parseFrontMatter(rawContent).body.size();
  std::string out(rawContent.substr(0, rawContent.size() - bodySize));
  out += newBody;
  return out;
}

std::optional<std::string> replaceFrontMatterId(std::string_view rawContent,
                                                std::string_view newId) {
  const auto split = splitFrontMatterBlock(rawContent);
  if (!split) return std::nullopt;
  const IdLines idLines =
      findTopLevelIdLines(rawContent, rawContent.find('\n') + 1, split->second);
  if (idLines.count != 1) return std::nullopt;

  std::string rewritten(rawContent.substr(0, idLines.valueStart));
  rewritten += ' ';
  rewritten += newId;
  rewritten += rawContent.substr(idLines.valueEnd);
  if (!differsOnlyInId(rawContent, rewritten, newId)) return std::nullopt;
  return rewritten;
}

std::optional<std::string> withFrontMatterId(std::string_view rawContent,
                                             std::string_view newId) {
  const auto split = splitFrontMatterBlock(rawContent);
  if (!split) return std::nullopt;
  const size_t openerEnd = rawContent.find('\n');
  if (findTopLevelIdLines(rawContent, openerEnd + 1, split->second).count != 0) {
    return replaceFrontMatterId(rawContent, newId);
  }

  const bool crlf = openerEnd > 0 && rawContent[openerEnd - 1] == '\r';
  std::string rewritten(rawContent.substr(0, openerEnd + 1));
  rewritten += "id: ";
  rewritten += newId;
  rewritten += crlf ? "\r\n" : "\n";
  rewritten += rawContent.substr(openerEnd + 1);
  if (!differsOnlyInId(rawContent, rewritten, newId)) return std::nullopt;
  return rewritten;
}

}  // namespace wikicore::vault
