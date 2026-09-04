#include "util/YouTubeEmbed.h"

#include <cctype>

namespace wikicore::util {

namespace {

bool isIdChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

std::string toLowerCopy(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Reads a run of ID characters starting at `pos`. Accepted only if it's
// EXACTLY 11 characters — a real YouTube video ID's length, always.
// Anything shorter or longer (capped at 12 read-ahead so a long garbage
// run doesn't scan the rest of the string) means this isn't a real ID:
// accepting a truncated or extended run here would mean either quietly
// embedding the wrong video or letting extra characters ride along into
// the URL this eventually builds.
std::optional<std::string> readExactly11IdChars(std::string_view s, size_t pos) {
  size_t end = pos;
  while (end < s.size() && isIdChar(s[end]) && (end - pos) < 12) ++end;
  if (end - pos != 11) return std::nullopt;
  return std::string(s.substr(pos, 11));
}

// Strips a leading scheme and a "www."/"m." subdomain, if present, so
// every host match below is a single plain prefix regardless of how the
// URL happened to be written (bare, http, https, with or without the
// subdomain a browser address bar or a phone's share sheet tends to add).
std::string_view stripSchemeAndSubdomain(std::string_view url) {
  for (std::string_view prefix : {"https://", "http://"}) {
    if (url.substr(0, prefix.size()) == prefix) {
      url.remove_prefix(prefix.size());
      break;
    }
  }
  for (std::string_view prefix : {"www.", "m."}) {
    if (url.substr(0, prefix.size()) == prefix) {
      url.remove_prefix(prefix.size());
      break;
    }
  }
  return url;
}

// "v=ID" can sit anywhere in a "?a=b&v=ID&c=d"-shaped query string —
// YouTube's own share links routinely append "&si=..."/"&t=..." on
// either side of it, not necessarily first or alone.
std::optional<std::string> findVParam(std::string_view query) {
  size_t pos = 0;
  while (pos < query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string_view param =
        amp == std::string_view::npos ? query.substr(pos) : query.substr(pos, amp - pos);
    if (param.substr(0, 2) == "v=") return readExactly11IdChars(param, 2);
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> extractYouTubeVideoId(std::string_view url) {
  const std::string_view stripped = stripSchemeAndSubdomain(url);

  constexpr std::string_view kWatch = "youtube.com/watch";
  constexpr std::string_view kShorts = "youtube.com/shorts/";
  constexpr std::string_view kEmbed = "youtube.com/embed/";
  constexpr std::string_view kShortLink = "youtu.be/";

  if (stripped.substr(0, kWatch.size()) == kWatch) {
    // Require the match to actually END the path segment ("watch" or
    // "watch?...", never "watch_something_else") before trusting it.
    if (stripped.size() > kWatch.size() && stripped[kWatch.size()] != '?') return std::nullopt;
    const size_t q = stripped.find('?');
    if (q == std::string_view::npos) return std::nullopt;  // no query string, no v= possible
    return findVParam(stripped.substr(q + 1));
  }
  if (stripped.substr(0, kShorts.size()) == kShorts) {
    return readExactly11IdChars(stripped, kShorts.size());
  }
  if (stripped.substr(0, kEmbed.size()) == kEmbed) {
    return readExactly11IdChars(stripped, kEmbed.size());
  }
  if (stripped.substr(0, kShortLink.size()) == kShortLink) {
    return readExactly11IdChars(stripped, kShortLink.size());
  }
  return std::nullopt;
}

std::string rewriteYouTubeEmbeds(std::string_view markdown) {
  std::string out;
  out.reserve(markdown.size());
  size_t pos = 0;
  while (true) {
    const size_t open = markdown.find("![", pos);
    if (open == std::string_view::npos) {
      out.append(markdown.substr(pos));
      break;
    }
    out.append(markdown.substr(pos, open - pos));

    const size_t altClose = markdown.find(']', open + 2);
    if (altClose == std::string_view::npos) {
      out.append(markdown.substr(open));
      break;
    }
    const std::string_view alt = markdown.substr(open + 2, altClose - (open + 2));

    // Alt text must be EXACTLY "youtube" (case-insensitive), immediately
    // followed by '(' with no space — anything else is a normal image,
    // not an embed request, and is left untouched.
    if (toLowerCopy(alt) != "youtube" || altClose + 1 >= markdown.size() ||
        markdown[altClose + 1] != '(') {
      out.append(markdown.substr(open, altClose + 1 - open));
      pos = altClose + 1;
      continue;
    }

    const size_t urlStart = altClose + 2;
    const size_t urlEnd = markdown.find(')', urlStart);
    if (urlEnd == std::string_view::npos) {
      out.append(markdown.substr(open));
      break;
    }
    const std::string_view inner = markdown.substr(urlStart, urlEnd - urlStart);
    // A CommonMark image may carry an optional 'title' after the URL
    // ("url \"title\"") — only the URL matters for embed detection.
    const size_t space = inner.find_first_of(" \t\n");
    const std::string_view rawUrl = space == std::string_view::npos ? inner : inner.substr(0, space);

    if (const auto videoId = extractYouTubeVideoId(rawUrl)) {
      out.append("![](youtube-embed:").append(*videoId).append(")");
    } else {
      // Not a URL shape recognized as YouTube — leave the original text
      // exactly as written rather than guess; it renders as a normal
      // (harmlessly broken) image link.
      out.append(markdown.substr(open, urlEnd + 1 - open));
    }
    pos = urlEnd + 1;
  }
  return out;
}

}  // namespace wikicore::util
