#pragma once

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

}  // namespace wikicore::vault
