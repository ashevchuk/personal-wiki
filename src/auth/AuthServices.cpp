#include "auth/AuthServices.h"

namespace wikicore::auth {

SessionStore* AuthServices::sessions_ = nullptr;
RateLimiter* AuthServices::rateLimiter_ = nullptr;
AdminAccount* AuthServices::admin_ = nullptr;

}  // namespace wikicore::auth
