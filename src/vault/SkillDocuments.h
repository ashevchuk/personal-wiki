#pragma once

#include <stdexcept>
#include <string_view>

namespace wikicore::vault {

// Documents with front-matter `type: skill` are served to MCP clients as
// skills: instructions that steer later model sessions. No model-facing write
// path (MCP tools over stdio or HTTP, the remote agent) may create one, edit
// one, turn a document into one, or attach files to one; skills are authored
// through the web UI only. The web routes do not use this check.
inline constexpr std::string_view kSkillDocumentType = "skill";

// Case-insensitive, surrounding ASCII whitespace ignored, so a hand-edited
// `type: Skill ` is still recognized as a skill everywhere.
bool isSkillType(std::string_view type);

class SkillDocumentReadOnlyError : public std::runtime_error {
 public:
  SkillDocumentReadOnlyError() : std::runtime_error("skill documents are read-only over MCP") {}
};

// Throws SkillDocumentReadOnlyError when `type` is a skill type. Called with
// the new type of a create/update and the existing type of an update/attach
// target.
void rejectSkillType(std::string_view type);

}  // namespace wikicore::vault
