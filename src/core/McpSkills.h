#pragma once

#include "core/McpProtocol.h"
#include "vault/SkillCatalog.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

// The MCP skills extension (io.modelcontextprotocol/skills, SEP-2640) on the
// 2026-07-28 path: skills/list, skills/get, resources/list, resources/read
// over vault::SkillCatalog. Results are built once here as nlohmann::ordered_json
// and served by both transports (the HTTP one converts to jsoncpp), so the
// wire shapes cannot drift apart. Results carry resultType and the caching
// hints; each transport adds its own `_meta` serverInfo.
namespace wikicore::mcp {

inline constexpr std::string_view kSkillsExtension = "io.modelcontextprotocol/skills";
inline constexpr std::string_view kSkillUriScheme = "skill://";

// Skills are edited live in the wiki; a short TTL keeps clients close to
// what is on disk (a stale entry also shows up client-side as a digest
// mismatch, which makes the host re-fetch).
inline constexpr long long kSkillsTtlMs = 60'000;

// server/discover capabilities on the 2026-07-28 path.
nlohmann::ordered_json modernServerCapabilities();

// The methods this module answers.
bool isSkillsMethod(std::string_view method);

struct MethodOutcome {
  nlohmann::ordered_json result;                // set when `error` is not
  std::optional<ProtocolError> error;  // -32602 for unknown skills/resources/cursors
};

MethodOutcome handleSkillsMethod(std::string_view method, const nlohmann::ordered_json& params,
                                 const vault::SkillCatalog& catalog, bool includePrivate);

// "skill://<name>/<path>".
std::string skillFileUri(const vault::Skill& skill, const vault::SkillFile& file);

// True when every byte sequence is well-formed UTF-8 (no overlongs,
// surrogates or code points above U+10FFFF).
bool isValidUtf8(std::string_view s);

}  // namespace wikicore::mcp
