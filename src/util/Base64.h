#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wikicore::util {

// RFC 4648 §4 standard alphabet (`+/`, padding `=`). `decodeBase64` also
// accepts the URL-safe alphabet (`-_`) — LLMs emit either — and strips
// ASCII whitespace first so a payload wrapped across lines still
// round-trips. Anything else invalid (bad character, bad padding) returns
// nullopt rather than a silently-truncated buffer.
std::string encodeBase64(std::string_view raw);
std::optional<std::string> decodeBase64(std::string_view encoded);

// If `encoded` is a data URL (`data:<type>;base64,<payload>`), returns
// the payload portion; otherwise returns `encoded` unchanged. The
// "data:" / "base64," markers are matched case-insensitively. The
// returned view points into `encoded`.
std::string_view stripDataUrlPrefix(std::string_view encoded);

}  // namespace wikicore::util
