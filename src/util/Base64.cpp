#include "util/Base64.h"

#include <array>
#include <cctype>

namespace wikicore::util {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr unsigned char kInvalid = 255;

constexpr std::array<unsigned char, 256> makeDecodeTable() {
  std::array<unsigned char, 256> t{};
  t.fill(kInvalid);
  for (unsigned char i = 0; i < 64; ++i) {
    t[static_cast<unsigned char>(kAlphabet[i])] = i;
  }
  // URL-safe alphabet — same 6-bit values as +/.
  t[static_cast<unsigned char>('-')] = t[static_cast<unsigned char>('+')];
  t[static_cast<unsigned char>('_')] = t[static_cast<unsigned char>('/')];
  return t;
}

constexpr auto kDecode = makeDecodeTable();

bool isAsciiWs(unsigned char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string encodeBase64(std::string_view raw) {
  const size_t n = raw.size();
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= n) {
    const unsigned int v = (static_cast<unsigned char>(raw[i]) << 16) |
                            (static_cast<unsigned char>(raw[i + 1]) << 8) |
                            static_cast<unsigned char>(raw[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back(kAlphabet[v & 63]);
    i += 3;
  }
  if (i < n) {
    unsigned int v = static_cast<unsigned char>(raw[i]) << 16;
    if (i + 1 < n) v |= static_cast<unsigned char>(raw[i + 1]) << 8;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    if (i + 1 < n) {
      out.push_back(kAlphabet[(v >> 6) & 63]);
      out.push_back('=');
    } else {
      out.push_back('=');
      out.push_back('=');
    }
  }
  return out;
}

std::optional<std::string> decodeBase64(std::string_view encoded) {
  // Collect 6-bit values, skipping whitespace; `=` ends the stream.
  std::string sextets;
  sextets.reserve(encoded.size());
  bool sawPad = false;
  int padCount = 0;
  for (unsigned char c : encoded) {
    if (isAsciiWs(c)) continue;
    if (c == '=') {
      sawPad = true;
      ++padCount;
      if (padCount > 2) return std::nullopt;
      continue;
    }
    if (sawPad) return std::nullopt;  // non-pad after padding
    const unsigned char v = kDecode[c];
    if (v == kInvalid) return std::nullopt;
    sextets.push_back(static_cast<char>(v));
  }

  const size_t n = sextets.size();
  // A complete group is 4 sextets -> 3 bytes. 2 sextets -> 1 byte (one
  // pad), 3 sextets -> 2 bytes (two pads). 1 leftover sextet is invalid.
  if (n % 4 == 1) return std::nullopt;
  // Padding count, when present, has to match the leftover sextet count:
  // 0 leftover -> 0 pads, 2 leftover -> 2 pads (or omitted), 3 leftover
  // -> 1 pad (or omitted). Accept omitted padding (URL-safe often skips
  // it) as well as the canonical form.
  if (padCount != 0) {
    if (n % 4 == 0 && padCount != 0) return std::nullopt;
    if (n % 4 == 2 && padCount != 2) return std::nullopt;
    if (n % 4 == 3 && padCount != 1) return std::nullopt;
  }

  std::string out;
  out.reserve((n / 4) * 3 + 2);
  size_t i = 0;
  while (i + 4 <= n) {
    const unsigned int v = (static_cast<unsigned char>(sextets[i]) << 18) |
                            (static_cast<unsigned char>(sextets[i + 1]) << 12) |
                            (static_cast<unsigned char>(sextets[i + 2]) << 6) |
                            static_cast<unsigned char>(sextets[i + 3]);
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
    i += 4;
  }
  if (n % 4 == 2) {
    const unsigned int v = (static_cast<unsigned char>(sextets[i]) << 18) |
                            (static_cast<unsigned char>(sextets[i + 1]) << 12);
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
  } else if (n % 4 == 3) {
    const unsigned int v = (static_cast<unsigned char>(sextets[i]) << 18) |
                            (static_cast<unsigned char>(sextets[i + 1]) << 12) |
                            (static_cast<unsigned char>(sextets[i + 2]) << 6);
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }
  return out;
}

std::string_view stripDataUrlPrefix(std::string_view encoded) {
  if (!startsWithIgnoreCase(encoded, "data:")) return encoded;
  constexpr std::string_view kMarker = "base64,";
  for (size_t i = 0; i + kMarker.size() <= encoded.size(); ++i) {
    if (startsWithIgnoreCase(encoded.substr(i, kMarker.size()), kMarker)) {
      return encoded.substr(i + kMarker.size());
    }
  }
  return encoded;
}

}  // namespace wikicore::util
