#include "core/McpSkills.h"

#include "util/Base64.h"

namespace wikicore::mcp {

namespace {

using json = nlohmann::ordered_json;

ProtocolError invalidParams(std::string message) {
  return ProtocolError{kErrorInvalidParams, std::move(message), {}};
}

std::string cacheScope(bool containsPrivate) { return containsPrivate ? "private" : "public"; }

json completeResult(bool containsPrivate) {
  return json{{"resultType", "complete"},
              {"ttlMs", kSkillsTtlMs},
              {"cacheScope", cacheScope(containsPrivate)}};
}

json skillEntry(const vault::Skill& skill) {
  json resources = json::array();
  for (const auto& file : skill.files) {
    resources.push_back(
        json{{"uri", skillFileUri(skill, file)}, {"digest", file.digest}, {"size", file.bytes.size()}});
  }
  return json{{"uri", skillFileUri(skill, skill.files.front())},
              {"frontmatter", json{{"name", skill.name}, {"description", skill.description}}},
              {"resources", std::move(resources)}};
}

// "skill://<name>/<path>" -> {name, path}; nullopt for any other shape.
std::optional<std::pair<std::string, std::string>> splitSkillUri(std::string_view uri) {
  if (uri.substr(0, kSkillUriScheme.size()) != kSkillUriScheme) return std::nullopt;
  uri.remove_prefix(kSkillUriScheme.size());
  const std::size_t slash = uri.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= uri.size()) {
    return std::nullopt;
  }
  return std::make_pair(std::string(uri.substr(0, slash)), std::string(uri.substr(slash + 1)));
}

std::optional<std::string> stringParam(const json& params, const char* key) {
  if (!params.is_object() || !params.contains(key) || !params[key].is_string()) {
    return std::nullopt;
  }
  return params[key].get<std::string>();
}

// The catalog is returned whole, so no cursor is ever issued; one the
// server never handed out is invalid.
bool hasCursor(const json& params) { return params.is_object() && params.contains("cursor"); }

bool listingHasPrivate(const vault::SkillListing& listing) {
  for (const auto& s : listing.skills) {
    if (!s.isPublic) return true;
  }
  return false;
}

MethodOutcome skillsList(const json& params, const vault::SkillCatalog& catalog,
                         bool includePrivate) {
  if (hasCursor(params)) return {{}, invalidParams("invalid cursor")};
  const vault::SkillListing listing = catalog.load(includePrivate);
  json result = completeResult(listingHasPrivate(listing));
  result["skills"] = json::array();
  for (const auto& skill : listing.skills) result["skills"].push_back(skillEntry(skill));
  return {std::move(result), std::nullopt};
}

MethodOutcome skillsGet(const json& params, const vault::SkillCatalog& catalog,
                        bool includePrivate) {
  const auto uri = stringParam(params, "uri");
  if (!uri) return {{}, invalidParams("missing 'uri'")};
  const auto parts = splitSkillUri(*uri);
  const auto skill = parts && parts->second == vault::kSkillMarkdownFile
                         ? catalog.find(parts->first, includePrivate)
                         : std::nullopt;
  if (!skill) return {{}, invalidParams("No skill is served at " + *uri)};
  json result = completeResult(!skill->isPublic);
  result["skill"] = skillEntry(*skill);
  return {std::move(result), std::nullopt};
}

MethodOutcome resourcesList(const json& params, const vault::SkillCatalog& catalog,
                            bool includePrivate) {
  if (hasCursor(params)) return {{}, invalidParams("invalid cursor")};
  const vault::SkillListing listing = catalog.load(includePrivate);
  json resources = json::array();
  for (const auto& skill : listing.skills) {
    for (const auto& file : skill.files) {
      const bool isSkillMd = &file == &skill.files.front();
      json r{{"uri", skillFileUri(skill, file)},
             {"name", isSkillMd ? skill.name : file.path.substr(file.path.rfind('/') + 1)},
             {"mimeType", file.mimeType},
             {"size", file.bytes.size()}};
      if (isSkillMd) r["description"] = skill.description;
      resources.push_back(std::move(r));
    }
  }
  json result = completeResult(listingHasPrivate(listing));
  result["resources"] = std::move(resources);
  return {std::move(result), std::nullopt};
}

MethodOutcome resourcesRead(const json& params, const vault::SkillCatalog& catalog,
                            bool includePrivate) {
  const auto uri = stringParam(params, "uri");
  if (!uri) return {{}, invalidParams("missing 'uri'")};
  const auto parts = splitSkillUri(*uri);
  const auto skill = parts ? catalog.find(parts->first, includePrivate) : std::nullopt;
  const vault::SkillFile* file = nullptr;
  if (skill) {
    for (const auto& f : skill->files) {
      if (f.path == parts->second) file = &f;
    }
  }
  if (!file) return {{}, invalidParams("Resource not found: " + *uri)};

  json content{{"uri", *uri}, {"mimeType", file->mimeType}};
  const bool isText = file->mimeType.rfind("text/", 0) == 0 && isValidUtf8(file->bytes);
  if (isText) {
    content["text"] = file->bytes;
  } else {
    content["blob"] = util::encodeBase64(file->bytes);
  }
  json result = completeResult(!skill->isPublic);
  result["contents"] = json::array({std::move(content)});
  return {std::move(result), std::nullopt};
}

}  // namespace

nlohmann::ordered_json modernServerCapabilities() {
  return json{{"tools", json::object()},
              {"resources", json::object()},
              {"extensions", json{{std::string(kSkillsExtension), json::object()}}}};
}

bool isSkillsMethod(std::string_view method) {
  return method == "skills/list" || method == "skills/get" || method == "resources/list" ||
         method == "resources/read";
}

MethodOutcome handleSkillsMethod(std::string_view method, const nlohmann::ordered_json& params,
                                 const vault::SkillCatalog& catalog, bool includePrivate) {
  if (method == "skills/list") return skillsList(params, catalog, includePrivate);
  if (method == "skills/get") return skillsGet(params, catalog, includePrivate);
  if (method == "resources/list") return resourcesList(params, catalog, includePrivate);
  return resourcesRead(params, catalog, includePrivate);
}

std::string skillFileUri(const vault::Skill& skill, const vault::SkillFile& file) {
  return std::string(kSkillUriScheme) + skill.name + "/" + file.path;
}

bool isValidUtf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len;
    char32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                          (len == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

}  // namespace wikicore::mcp
