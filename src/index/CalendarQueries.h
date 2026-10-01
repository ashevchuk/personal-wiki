#pragma once

#include "index/Database.h"

#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct CalendarEvent {
  std::string path;
  std::string title;
  std::string visibility;
  std::string date;  // ISO8601 YYYY-MM-DD -- the concrete occurrence date,
                      // not necessarily the document's own `due` (a
                      // recurring series' Nth occurrence lands here)
};

// Recurrence rule grammar, same whitelisted-DSL-not-raw-anything
// discipline as QueryBlocks' own `key: value` grammar -- one bare
// frequency token, then optional ';'-separated 'key=value' modifiers:
//
//   daily | weekly | monthly | yearly
//   ;interval=N      -- every N periods instead of every 1 (default 1)
//   ;until=YYYY-MM-DD -- stop generating occurrences after this date
//   ;count=N          -- stop after N occurrences
//
// `until` and `count` may both be given -- whichever bound is hit FIRST
// stops expansion (an AND, not an error to combine); omitting both means
// "no end", bounded in practice by whatever date range the caller asked
// for. Meaningless without `due` set on the same document (the anchor
// date for the series) -- see FrontMatter.h's own comment. A `recur`
// string that doesn't start with one of the four frequency words is
// simply treated as "not recurring" (parseRecurrenceRule returns
// nullopt) -- same fail-safe-skip-rather-than-throw precedent as a
// malformed `due`, not QueryBlocks' own hard-parse-error discipline:
// this is scanning the VAULT'S OWN STORED DATA across many documents at
// once, where one bad row must never take the whole calendar down with
// it, not validating a single admin-typed query block.
struct RecurrenceRule {
  enum class Frequency { kDaily, kWeekly, kMonthly, kYearly };
  Frequency frequency = Frequency::kDaily;
  int interval = 1;
  std::string until;  // ISO8601 date, or "" for no bound
  int count = 0;       // 0 means no bound
};

// Returns nullopt for an empty or unrecognized string -- see the grammar
// comment above.
std::optional<RecurrenceRule> parseRecurrenceRule(const std::string& recur);

// Read-only, visibility-gated (same fail-safe-private direction as
// NavQueries) calendar backend: every document carrying a `due` date,
// recurring series expanded into concrete occurrences, restricted to
// [startDate, endDate] inclusive (both ISO8601 YYYY-MM-DD). `folder`
// non-empty scopes to a path prefix, same convention as QueryBlocks'
// own `folder:` -- this plus `tag` are how "multiple calendars" works in
// this app: there's no separate calendar entity, just this same filter
// applied to a different tag/folder (see docs/calendar.md).
class CalendarQueries {
 public:
  explicit CalendarQueries(Database& db) : db_(db) {}

  std::vector<CalendarEvent> eventsBetween(const std::string& startDate,
                                            const std::string& endDate,
                                            bool includePrivate,
                                            const std::string& folder = "",
                                            const std::vector<std::string>& tags = {}) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
