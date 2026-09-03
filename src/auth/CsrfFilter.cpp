#include "auth/CsrfFilter.h"

#include "auth/AuthContext.h"
#include "auth/AuthServices.h"

#include <drogon/HttpResponse.h>

using namespace drogon;

namespace wikicore::auth {

namespace {

// Not a timing-side-channel-critical comparison in absolute terms (an
// attacker who can already read response timing on a personal, low-
// traffic wiki has bigger problems), but it costs nothing to not leak the
// prefix length of a match, so we don't.
bool constantTimeEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace

void CsrfFilter::doFilter(const HttpRequestPtr& req, FilterCallback&& fcb,
                           FilterChainCallback&& fccb) {
  const std::string& token = req->getCookie(kSessionCookieName);
  if (token.empty()) {
    fccb();  // no session -> not this filter's concern, see header comment
    return;
  }

  const auto info = AuthServices::sessions().validate(token);
  if (!info) {
    fccb();
    return;
  }

  std::string submitted = req->getHeader(kCsrfHeaderName);
  if (submitted.empty()) {
    submitted = req->getParameter(kCsrfFormField);
  }

  if (!constantTimeEquals(submitted, info->csrfToken)) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k403Forbidden);
    resp->setContentTypeCode(CT_TEXT_PLAIN);
    resp->setBody("csrf token missing or invalid\n");
    fcb(resp);
    return;
  }

  fccb();
}

}  // namespace wikicore::auth
