#include "auth/ClientIp.h"

#include <cctype>

namespace wikicore::auth {

namespace {

std::string trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(start, end - start);
}

}  // namespace

std::string clientIp(const drogon::HttpRequestPtr& req) {
  const std::string xRealIp = req->getHeader("X-Real-IP");
  if (!xRealIp.empty()) return trim(xRealIp);

  // LAST entry, not first — see this function's own header comment for
  // exactly why the last one is nginx's own (trusted) append and the
  // first is whatever the client itself claimed.
  const std::string xff = req->getHeader("X-Forwarded-For");
  if (!xff.empty()) {
    const size_t comma = xff.rfind(',');
    return trim(comma == std::string::npos ? xff : xff.substr(comma + 1));
  }

  return req->getPeerAddr().toIp();
}

}  // namespace wikicore::auth
