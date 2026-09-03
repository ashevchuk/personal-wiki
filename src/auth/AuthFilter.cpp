#include "auth/AuthFilter.h"

#include "auth/AuthContext.h"
#include "auth/AuthServices.h"

#include <optional>

using namespace drogon;

namespace wikicore::auth {

void AuthFilter::doFilter(const HttpRequestPtr& req, FilterCallback&&,
                           FilterChainCallback&& fccb) {
  std::optional<int64_t> userId;
  std::string csrfToken;

  const std::string& token = req->getCookie(kSessionCookieName);
  if (!token.empty()) {
    if (auto info = AuthServices::sessions().validate(token)) {
      userId = info->userId;
      csrfToken = info->csrfToken;
    }
  }

  req->attributes()->insert(kAttrUserId, userId);
  req->attributes()->insert(kAttrCsrfToken, csrfToken);
  fccb();
}

}  // namespace wikicore::auth
