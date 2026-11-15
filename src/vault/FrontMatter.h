#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikicore::vault {

struct FrontMatter {
  std::string id;
  std::string title;
  std::vector<std::string> tags;
  // Fail-safe: anything missing, malformed, or not exactly "public"
  // defaults to "private". See docs/architecture.md /
  // "Visibility is fail-safe-private" in CLAUDE.md.
  std::string visibility = "private";
  std::string type;
  std::string created;
  std::string updated;
  // Calendar fields -- both plain, unvalidated strings at this layer,
  // same convention as `type`: free text on write, parsed (and silently
  // skipped if unparseable, never a hard error) only at calendar-query
  // time. `due` is a bare ISO8601 date (YYYY-MM-DD); a document with no
  // date leaves this empty and simply never appears in a calendar/agenda
  // result. `recur` is CalendarQueries' own small whitelisted DSL (see
  // that file), meaningless without `due` set -- a `recur` with no `due`
  // is inert, not an error, same "fields don't cross-validate each
  // other" precedent as `visibility`/`type` already not depending on one
  // another.
  std::string due;
  std::string recur;
};

struct ParsedDocument {
  FrontMatter frontMatter;
  std::string body;  // markdown content after the front-matter block
};

// Parses a `---`-delimited YAML front-matter block off the front of
// `rawContent`, if present. A missing, malformed, or unparseable block is
// NOT an error: it's treated as "no front matter", the whole input becomes
// `body`, and `frontMatter` is left at its fail-safe defaults (title/id
// empty — callers fill those in from the filename/a fresh UUID; visibility
// stays "private"). This function never throws.
ParsedDocument parseFrontMatter(std::string_view rawContent);

// The inverse of parseFrontMatter: renders `fm` as a `---`-delimited YAML
// block (via YAML::Emitter, so titles/tags containing colons, quotes, etc.
// come out correctly quoted) followed by `body`. Round-trips with
// parseFrontMatter for any FrontMatter produced by this codebase — this is
// what DocumentService writes to disk.
std::string serializeFrontMatter(const FrontMatter& fm, const std::string& body);

// Same, for rewriting an existing file: top-level keys of `existingRaw`'s
// front matter that FrontMatter does not model are emitted after the
// modeled ones, so a hand-written key survives a save. YAML comments do not.
std::string serializeFrontMatter(const FrontMatter& fm, const std::string& body,
                                 std::string_view existingRaw);

// `rawContent` with its body (as parseFrontMatter defines it) replaced by
// `newBody` and the front-matter block before it kept byte for byte.
std::string replaceBody(std::string_view rawContent, std::string_view newBody);

// Returns `rawContent` with the value of its top-level `id:` line replaced
// by `newId` and every other byte untouched, so keys FrontMatter does not
// model survive (serializeFrontMatter would drop them). nullopt when the
// block has no single plain `id:` line to rewrite (flow-style mapping,
// multi-line scalar, no id) or when re-parsing shows anything besides the
// id changed.
std::optional<std::string> replaceFrontMatterId(std::string_view rawContent,
                                                std::string_view newId);

// replaceFrontMatterId, or, when the block has no `id:` line, an `id:` line
// inserted right after the opening delimiter. Same nullopt contract.
std::optional<std::string> withFrontMatterId(std::string_view rawContent,
                                             std::string_view newId);

// The editable fields of an uploaded .md file. A field is set only when the
// file's front matter actually has that key, so a caller can leave its own
// value alone for a key the file has no opinion about (unlike FrontMatter,
// whose defaults can't tell "absent" from "empty").
struct ImportedFields {
  std::optional<std::string> title;
  std::optional<std::string> type;
  std::optional<std::string> visibility;  // "public" or "private" only
  std::optional<std::string> due;
  std::optional<std::string> recur;
  std::optional<std::vector<std::string>> tags;
};

struct ImportedDocument {
  ImportedFields fields;
  std::string body;
};

// Reads an externally supplied .md file with the same YAML parser that reads
// the vault, so a file saved by serializeFrontMatter or by another tool
// decodes identically to how it would on disk. Like parseFrontMatter, a
// missing, malformed, or non-mapping front-matter block is not an error: no
// field is set and the whole input is the body. Never throws.
ImportedDocument parseForImport(std::string_view rawContent);

}  // namespace wikicore::vault
