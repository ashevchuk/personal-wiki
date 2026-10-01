#include "index/CalendarQueries.h"

#include "index/Statement.h"

#include <chrono>
#include <cstdio>
#include <functional>

namespace wikicore::index {

namespace {

namespace chr = std::chrono;

// Parses a strict "YYYY-MM-DD" string. Returns nullopt for anything else
// (wrong length, non-digits, an impossible calendar date like 2026-02-30)
// -- never throws, matching every other fail-safe parser in this codebase.
std::optional<chr::year_month_day> parseIsoDate(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  int y, m, d;
  if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return std::nullopt;
  const chr::year_month_day ymd{chr::year{y}, chr::month{static_cast<unsigned>(m)},
                                 chr::day{static_cast<unsigned>(d)}};
  if (!ymd.ok()) return std::nullopt;
  return ymd;
}

std::string formatIsoDate(chr::year_month_day ymd) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                               static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return std::string(buf, buf + (n > 0 ? n : 0));
}

// Adding calendar months/years to a year_month_day can land on a day
// that doesn't exist in the target month (Jan 31 + 1 month -> "Feb 31").
// std::chrono's own year_month_day + months/years arithmetic produces
// exactly that -- a value whose .ok() is false -- rather than clamping,
// so this does the clamp-to-last-valid-day-of-month every real calendar
// app does: Jan 31 "monthly" becomes Feb 28/29, Mar 31, Apr 30, ... not a
// skipped occurrence and not a thrown error.
chr::year_month_day clampToValidDay(chr::year_month_day ymd) {
  if (ymd.ok()) return ymd;
  return ymd.year() / ymd.month() / chr::last;
}

std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::vector<std::string> splitSemicolon(const std::string& s) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t next = s.find(';', pos);
    const std::string part =
        trim(next == std::string::npos ? s.substr(pos) : s.substr(pos, next - pos));
    if (!part.empty()) parts.push_back(part);
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return parts;
}

std::optional<int> parsePositiveInt(const std::string& s) {
  if (s.empty()) return std::nullopt;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  try {
    const int v = std::stoi(s);
    return v > 0 ? std::optional<int>(v) : std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// Advances `anchor` by `periods` occurrences of `freq` -- the Nth
// occurrence of the series (periods=0 returns anchor itself unchanged).
chr::year_month_day advance(chr::year_month_day anchor, RecurrenceRule::Frequency freq,
                             int periods) {
  using F = RecurrenceRule::Frequency;
  switch (freq) {
    case F::kDaily:
      return chr::year_month_day{chr::sys_days{anchor} + chr::days{periods}};
    case F::kWeekly:
      return chr::year_month_day{chr::sys_days{anchor} + chr::days{periods * 7}};
    case F::kMonthly:
      return clampToValidDay(anchor + chr::months{periods});
    case F::kYearly:
      return clampToValidDay(anchor + chr::years{periods});
  }
  return anchor;  // unreachable, silences -Wreturn-type
}

void appendTagFilter(std::vector<std::string>& whereClauses,
                      std::vector<std::function<void(Statement&, int)>>& binders,
                      const std::vector<std::string>& tags) {
  if (tags.empty()) return;
  std::string inList;
  for (size_t i = 0; i < tags.size(); ++i) {
    if (i) inList += ", ";
    inList += "?";
  }
  whereClauses.push_back(
      "d.rowid_id IN (SELECT dt.document_rowid FROM document_tags dt "
      "JOIN tags t ON t.id = dt.tag_id WHERE t.name IN (" +
      inList + ") GROUP BY dt.document_rowid HAVING COUNT(DISTINCT t.id) = " +
      std::to_string(tags.size()) + ")");
  for (const auto& tag : tags) {
    binders.push_back([tag](Statement& s, int idx) { s.bind(idx, tag); });
  }
}

std::string escapeLikePattern(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::optional<RecurrenceRule> parseRecurrenceRule(const std::string& recur) {
  const auto parts = splitSemicolon(recur);
  if (parts.empty()) return std::nullopt;

  RecurrenceRule rule;
  if (parts[0] == "daily") {
    rule.frequency = RecurrenceRule::Frequency::kDaily;
  } else if (parts[0] == "weekly") {
    rule.frequency = RecurrenceRule::Frequency::kWeekly;
  } else if (parts[0] == "monthly") {
    rule.frequency = RecurrenceRule::Frequency::kMonthly;
  } else if (parts[0] == "yearly") {
    rule.frequency = RecurrenceRule::Frequency::kYearly;
  } else {
    return std::nullopt;
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    const size_t eq = parts[i].find('=');
    if (eq == std::string::npos) continue;  // malformed modifier -- ignore, don't fail the row
    const std::string key = trim(parts[i].substr(0, eq));
    const std::string value = trim(parts[i].substr(eq + 1));
    if (key == "interval") {
      if (const auto v = parsePositiveInt(value)) rule.interval = *v;
    } else if (key == "until") {
      if (parseIsoDate(value)) rule.until = value;  // only keep it if it's a real date
    } else if (key == "count") {
      if (const auto v = parsePositiveInt(value)) rule.count = *v;
    }
  }
  return rule;
}

std::vector<CalendarEvent> CalendarQueries::eventsBetween(const std::string& startDate,
                                                           const std::string& endDate,
                                                           bool includePrivate,
                                                           const std::string& folder,
                                                           const std::vector<std::string>& tags) const {
  const auto start = parseIsoDate(startDate);
  const auto end = parseIsoDate(endDate);
  if (!start || !end || *end < *start) return {};

  std::vector<std::string> whereClauses = {
      "(? = 1 OR d.visibility = 'public')", "d.due_at != ''",
      "(d.due_at BETWEEN ? AND ? OR (d.recur != '' AND d.due_at <= ?))"};
  std::vector<std::function<void(Statement&, int)>> binders = {
      [includePrivate](Statement& s, int idx) {
        s.bind(idx, static_cast<int64_t>(includePrivate ? 1 : 0));
      },
      [startDate](Statement& s, int idx) { s.bind(idx, startDate); },
      [endDate](Statement& s, int idx) { s.bind(idx, endDate); },
      [endDate](Statement& s, int idx) { s.bind(idx, endDate); },
  };

  if (!folder.empty()) {
    whereClauses.push_back("d.path LIKE ? ESCAPE '\\'");
    const std::string pattern = escapeLikePattern(folder) + "%";
    binders.push_back([pattern](Statement& s, int idx) { s.bind(idx, pattern); });
  }
  appendTagFilter(whereClauses, binders, tags);

  std::string sql = "SELECT d.path, d.title, d.visibility, d.due_at, d.recur FROM documents d WHERE ";
  for (size_t i = 0; i < whereClauses.size(); ++i) {
    if (i) sql += " AND ";
    sql += whereClauses[i];
  }
  sql += ";";

  Statement stmt(db_.handle(), sql);
  for (size_t i = 0; i < binders.size(); ++i) {
    binders[i](stmt, static_cast<int>(i) + 1);
  }

  std::vector<CalendarEvent> events;
  while (stmt.step()) {
    const std::string path = stmt.columnText(0);
    const std::string title = stmt.columnText(1);
    const std::string visibility = stmt.columnText(2);
    const std::string dueAt = stmt.columnText(3);
    const std::string recur = stmt.columnText(4);

    const auto due = parseIsoDate(dueAt);
    if (!due) continue;  // malformed `due` on this one row -- skip it, not the whole calendar

    if (recur.empty()) {
      // Already confirmed BETWEEN start/end by the SQL itself.
      events.push_back(CalendarEvent{path, title, visibility, dueAt});
      continue;
    }

    const auto rule = parseRecurrenceRule(recur);
    if (!rule) {
      // Unrecognized recur text (a typo, most likely) -- fall back to
      // treating this document as a one-off on its own `due` date rather
      // than losing the event entirely. The SQL candidate filter above
      // only guaranteed due_at <= endDate for a recur-bearing row (it
      // doesn't know yet whether recur will turn out to be garbage), so
      // the lower bound still needs checking here.
      if (*due >= *start && *due <= *end) {
        events.push_back(CalendarEvent{path, title, visibility, dueAt});
      }
      continue;
    }

    // Walks the series from its own anchor one occurrence at a time --
    // fine for a personal-wiki-sized vault and date range, but a series
    // anchored years before `startDate` with a short interval (daily,
    // say) pays for every skipped occurrence just to reach the window.
    // A closed-form "jump to the first occurrence >= startDate" is
    // possible (plain arithmetic for daily/weekly, calendar-aware for
    // monthly/yearly) but not worth the complexity unless this is ever
    // measured as a real problem.
    const auto untilBound = rule->until.empty() ? std::nullopt : parseIsoDate(rule->until);
    for (int occurrence = 0; rule->count == 0 || occurrence < rule->count; ++occurrence) {
      const chr::year_month_day date = advance(*due, rule->frequency, occurrence * rule->interval);
      if (untilBound && date > *untilBound) break;
      if (date > *end) break;  // series only moves forward -- nothing later will be in range either
      if (date >= *start) {
        events.push_back(CalendarEvent{path, title, visibility, formatIsoDate(date)});
      }
    }
  }
  return events;
}

}  // namespace wikicore::index
