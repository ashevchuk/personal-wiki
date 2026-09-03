#pragma once

#include <string>

namespace wikicore::util {

// A random (v4) UUID, lowercase hyphenated form. Deliberately hand-rolled
// (getrandom()-backed, same CSPRNG approach as PasswordHasher/SessionStore)
// rather than reaching for drogon::utils::getUuid() — this lives in
// wikicore, which stays free of any Drogon dependency (see
// docs/architecture.md), and a UUID is about a dozen lines over RFC 4122.
std::string newUuidV4();

}  // namespace wikicore::util
