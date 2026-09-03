#pragma once

#include <string>

namespace wikicore::util {

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ" — the format used for every
// timestamp column/front-matter field in this project, so they sort
// lexicographically and compare correctly as plain TEXT in SQLite.
std::string nowIso8601();

// `nowIso8601()` plus `seconds` — used for session expiry.
std::string isoTimestampAfter(long seconds);

}  // namespace wikicore::util
