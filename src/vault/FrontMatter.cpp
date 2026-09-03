#include "vault/FrontMatter.h"

#include <yaml-cpp/yaml.h>

#include <optional>
#include <utility>

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
  fm.created = readScalarOr(doc["created"], "");
  fm.updated = readScalarOr(doc["updated"], "");

  // The one field where "parsed successfully but not what we expect"
  // must still collapse to the safe default, not pass through verbatim.
  const std::string rawVisibility = readScalarOr(doc["visibility"], "private");
  fm.visibility = (rawVisibility == "public") ? "public" : "private";

  result.frontMatter = std::move(fm);
  return result;
}

}  // namespace wikicore::vault
