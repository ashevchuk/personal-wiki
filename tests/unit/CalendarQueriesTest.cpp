#include "index/CalendarQueries.h"
#include "index/Database.h"
#include "index/IndexUpdater.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-calendar-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".db")) {
    fs::remove(path_);
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

DocumentIndexEntry makeEntry(std::string path, std::string visibility, std::string dueAt,
                              std::string recur = "") {
  DocumentIndexEntry e;
  e.uuid = path;
  e.title = path;
  e.path = std::move(path);
  e.visibility = std::move(visibility);
  e.createdAt = e.updatedAt = "2026-01-01T00:00:00Z";
  e.dueAt = std::move(dueAt);
  e.recur = std::move(recur);
  return e;
}

bool hasDate(const std::vector<CalendarEvent>& events, const std::string& path,
             const std::string& date) {
  return std::any_of(events.begin(), events.end(), [&](const CalendarEvent& e) {
    return e.path == path && e.date == date;
  });
}

}  // namespace

// --- parseRecurrenceRule (pure logic, no DB) --------------------------

TEST_CASE("parseRecurrenceRule accepts the four bare frequencies", "[CalendarQueries]") {
  REQUIRE(parseRecurrenceRule("daily")->frequency == RecurrenceRule::Frequency::kDaily);
  REQUIRE(parseRecurrenceRule("weekly")->frequency == RecurrenceRule::Frequency::kWeekly);
  REQUIRE(parseRecurrenceRule("monthly")->frequency == RecurrenceRule::Frequency::kMonthly);
  REQUIRE(parseRecurrenceRule("yearly")->frequency == RecurrenceRule::Frequency::kYearly);
}

TEST_CASE("parseRecurrenceRule rejects empty or unrecognized text", "[CalendarQueries]") {
  REQUIRE_FALSE(parseRecurrenceRule("").has_value());
  REQUIRE_FALSE(parseRecurrenceRule("fortnightly").has_value());
  REQUIRE_FALSE(parseRecurrenceRule("every tuesday").has_value());
}

TEST_CASE("parseRecurrenceRule reads interval/until/count modifiers", "[CalendarQueries]") {
  const auto rule = parseRecurrenceRule("weekly;interval=2;until=2027-01-01;count=5");
  REQUIRE(rule);
  REQUIRE(rule->frequency == RecurrenceRule::Frequency::kWeekly);
  REQUIRE(rule->interval == 2);
  REQUIRE(rule->until == "2027-01-01");
  REQUIRE(rule->count == 5);
}

TEST_CASE("parseRecurrenceRule ignores a malformed modifier instead of failing the "
          "whole rule",
          "[CalendarQueries]") {
  const auto rule = parseRecurrenceRule("daily;bogus;interval=3");
  REQUIRE(rule);
  REQUIRE(rule->interval == 3);
}

TEST_CASE("parseRecurrenceRule defaults interval to 1 and leaves until/count unbounded",
          "[CalendarQueries]") {
  const auto rule = parseRecurrenceRule("monthly");
  REQUIRE(rule);
  REQUIRE(rule->interval == 1);
  REQUIRE(rule->until.empty());
  REQUIRE(rule->count == 0);
}

// --- CalendarQueries::eventsBetween ------------------------------------

TEST_CASE("CalendarQueries: a one-off due date appears only when inside the window",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "2026-03-15"));

  CalendarQueries cal(database);
  REQUIRE(hasDate(cal.eventsBetween("2026-03-01", "2026-03-31", true), "a.md", "2026-03-15"));
  REQUIRE(cal.eventsBetween("2026-04-01", "2026-04-30", true).empty());
}

TEST_CASE("CalendarQueries: a document with no due date never appears", "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("no-date.md", "public", ""));

  CalendarQueries cal(database);
  REQUIRE(cal.eventsBetween("2026-01-01", "2026-12-31", true).empty());
}

TEST_CASE("CalendarQueries: visibility gates events the same fail-safe-private way "
          "as everything else",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("public-event.md", "public", "2026-06-01"));
  updater.upsertOne(makeEntry("private-event.md", "private", "2026-06-02"));

  CalendarQueries cal(database);
  const auto anon = cal.eventsBetween("2026-06-01", "2026-06-30", false);
  REQUIRE(hasDate(anon, "public-event.md", "2026-06-01"));
  REQUIRE_FALSE(hasDate(anon, "private-event.md", "2026-06-02"));

  const auto admin = cal.eventsBetween("2026-06-01", "2026-06-30", true);
  REQUIRE(hasDate(admin, "private-event.md", "2026-06-02"));
}

TEST_CASE("CalendarQueries: weekly recurrence expands multiple occurrences in range",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("standup.md", "public", "2026-03-02", "weekly"));

  CalendarQueries cal(database);
  const auto events = cal.eventsBetween("2026-03-01", "2026-03-31", true);
  REQUIRE(hasDate(events, "standup.md", "2026-03-02"));
  REQUIRE(hasDate(events, "standup.md", "2026-03-09"));
  REQUIRE(hasDate(events, "standup.md", "2026-03-16"));
  REQUIRE(hasDate(events, "standup.md", "2026-03-23"));
  REQUIRE(hasDate(events, "standup.md", "2026-03-30"));
  REQUIRE_FALSE(hasDate(events, "standup.md", "2026-04-06"));
}

TEST_CASE("CalendarQueries: an anchor date far before the window still expands "
          "into it",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("old-weekly.md", "public", "2020-01-06", "weekly"));

  CalendarQueries cal(database);
  // 2020-01-06 is a Monday; weekly from there lands on every Monday
  // forever, including ones in 2026.
  REQUIRE(hasDate(cal.eventsBetween("2026-03-01", "2026-03-31", true), "old-weekly.md",
                   "2026-03-02"));
}

TEST_CASE("CalendarQueries: monthly recurrence clamps Jan 31 to the last day of "
          "shorter months",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("month-end.md", "public", "2026-01-31", "monthly"));

  CalendarQueries cal(database);
  REQUIRE(hasDate(cal.eventsBetween("2026-02-01", "2026-02-28", true), "month-end.md",
                   "2026-02-28"));
  REQUIRE(hasDate(cal.eventsBetween("2026-03-01", "2026-03-31", true), "month-end.md",
                   "2026-03-31"));
  REQUIRE(hasDate(cal.eventsBetween("2026-04-01", "2026-04-30", true), "month-end.md",
                   "2026-04-30"));
}

TEST_CASE("CalendarQueries: yearly recurrence clamps Feb 29 on non-leap years",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("leap-birthday.md", "public", "2024-02-29", "yearly"));

  CalendarQueries cal(database);
  // 2026 is not a leap year.
  REQUIRE(hasDate(cal.eventsBetween("2026-02-01", "2026-02-28", true), "leap-birthday.md",
                   "2026-02-28"));
}

TEST_CASE("CalendarQueries: until stops expansion after the bound", "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("limited.md", "public", "2026-03-02", "weekly;until=2026-03-09"));

  CalendarQueries cal(database);
  const auto events = cal.eventsBetween("2026-03-01", "2026-03-31", true);
  REQUIRE(hasDate(events, "limited.md", "2026-03-02"));
  REQUIRE(hasDate(events, "limited.md", "2026-03-09"));
  REQUIRE_FALSE(hasDate(events, "limited.md", "2026-03-16"));
}

TEST_CASE("CalendarQueries: count stops expansion after N occurrences", "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("twice.md", "public", "2026-03-02", "weekly;count=2"));

  CalendarQueries cal(database);
  const auto events = cal.eventsBetween("2026-03-01", "2026-03-31", true);
  REQUIRE(hasDate(events, "twice.md", "2026-03-02"));
  REQUIRE(hasDate(events, "twice.md", "2026-03-09"));
  REQUIRE_FALSE(hasDate(events, "twice.md", "2026-03-16"));
}

TEST_CASE("CalendarQueries: interval=2 skips every other period", "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("biweekly.md", "public", "2026-03-02", "weekly;interval=2"));

  CalendarQueries cal(database);
  const auto events = cal.eventsBetween("2026-03-01", "2026-03-31", true);
  REQUIRE(hasDate(events, "biweekly.md", "2026-03-02"));
  REQUIRE_FALSE(hasDate(events, "biweekly.md", "2026-03-09"));
  REQUIRE(hasDate(events, "biweekly.md", "2026-03-16"));
  REQUIRE_FALSE(hasDate(events, "biweekly.md", "2026-03-23"));
  REQUIRE(hasDate(events, "biweekly.md", "2026-03-30"));
}

TEST_CASE("CalendarQueries: an unparseable recur string falls back to the single "
          "due date instead of losing the event entirely",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("bad-recur.md", "public", "2026-03-15", "fortnightly"));

  CalendarQueries cal(database);
  REQUIRE(hasDate(cal.eventsBetween("2026-03-01", "2026-03-31", true), "bad-recur.md",
                   "2026-03-15"));
  // Still just the one date -- a typo'd recur doesn't somehow fabricate
  // a real recurrence, it degrades to "no recurrence at all".
  REQUIRE(cal.eventsBetween("2026-04-01", "2026-04-30", true).empty());
}

TEST_CASE("CalendarQueries: folder filter scopes to one calendar", "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("work/standup.md", "public", "2026-03-02"));
  updater.upsertOne(makeEntry("personal/dentist.md", "public", "2026-03-03"));

  CalendarQueries cal(database);
  const auto workOnly = cal.eventsBetween("2026-03-01", "2026-03-31", true, "work/");
  REQUIRE(hasDate(workOnly, "work/standup.md", "2026-03-02"));
  REQUIRE_FALSE(hasDate(workOnly, "personal/dentist.md", "2026-03-03"));
}

TEST_CASE("CalendarQueries: an invalid date range returns nothing rather than "
          "throwing",
          "[CalendarQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  CalendarQueries cal(database);
  REQUIRE(cal.eventsBetween("not-a-date", "2026-03-31", true).empty());
  REQUIRE(cal.eventsBetween("2026-03-31", "2026-03-01", true).empty());  // end before start
}
