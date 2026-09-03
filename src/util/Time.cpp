#include "util/Time.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace wikicore::util {

namespace {

std::string formatIso8601(std::time_t t) {
  std::tm utc{};
  gmtime_r(&t, &utc);
  std::ostringstream oss;
  oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

}  // namespace

std::string nowIso8601() {
  return formatIso8601(std::time(nullptr));
}

std::string isoTimestampAfter(long seconds) {
  return formatIso8601(std::time(nullptr) + seconds);
}

}  // namespace wikicore::util
