#include "auth/SessionStore.h"

#include "util/Time.h"

#include <openssl/evp.h>
#include <sqlite3.h>
#include <sys/random.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace wikicore::auth {

namespace {

std::string toHex(const unsigned char* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    oss << std::setw(2) << static_cast<int>(data[i]);
  }
  return oss.str();
}

// 32 random bytes, hex-encoded (64 chars) — used for both the session
// token and the CSRF token. See PasswordHasher::randomSalt for why
// getrandom() over std::random_device.
std::string randomHexToken() {
  std::array<unsigned char, 32> bytes{};
  ssize_t got = 0;
  while (got < static_cast<ssize_t>(bytes.size())) {
    const ssize_t n = getrandom(bytes.data() + got, bytes.size() - got, 0);
    if (n < 0) throw std::runtime_error("getrandom() failed for token");
    got += n;
  }
  return toHex(bytes.data(), bytes.size());
}

std::string sha256Hex(const std::string& input) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  size_t digestLen = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(),
                    digest, &digestLen) != 1) {
    throw std::runtime_error("EVP_Q_digest(SHA256) failed");
  }
  return toHex(digest, digestLen);
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

NewSession SessionStore::create(int64_t userId, const std::string& userAgent,
                                 const std::string& ip, long ttlSeconds) {
  pruneExpired();

  NewSession session;
  session.rawToken = randomHexToken();
  session.csrfToken = randomHexToken();

  static constexpr const char* kSql =
      "INSERT INTO sessions(token_hash, user_id, created_at, expires_at, "
      "  last_seen_at, csrf_token, user_agent, ip) "
      "VALUES (?1, ?2, ?3, ?4, ?3, ?5, ?6, ?7);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  bindText(stmt, 1, sha256Hex(session.rawToken));
  sqlite3_bind_int64(stmt, 2, userId);
  bindText(stmt, 3, wikicore::util::nowIso8601());
  bindText(stmt, 4, wikicore::util::isoTimestampAfter(ttlSeconds));
  bindText(stmt, 5, session.csrfToken);
  bindText(stmt, 6, userAgent);
  bindText(stmt, 7, ip);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("failed to create session: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  return session;
}

std::optional<SessionInfo> SessionStore::validate(const std::string& rawToken) {
  static constexpr const char* kSql =
      "SELECT user_id, csrf_token FROM sessions "
      "WHERE token_hash = ?1 AND expires_at > ?2;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  const std::string tokenHash = sha256Hex(rawToken);
  bindText(stmt, 1, tokenHash);
  bindText(stmt, 2, wikicore::util::nowIso8601());

  std::optional<SessionInfo> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    SessionInfo info;
    info.userId = sqlite3_column_int64(stmt, 0);
    info.csrfToken =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    result = std::move(info);
  }
  sqlite3_finalize(stmt);

  if (result) {
    static constexpr const char* kTouch =
        "UPDATE sessions SET last_seen_at = ?1 WHERE token_hash = ?2;";
    sqlite3_stmt* touchStmt = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), kTouch, -1, &touchStmt, nullptr) ==
        SQLITE_OK) {
      bindText(touchStmt, 1, wikicore::util::nowIso8601());
      bindText(touchStmt, 2, tokenHash);
      sqlite3_step(touchStmt);
      sqlite3_finalize(touchStmt);
    }
  }
  return result;
}

void SessionStore::destroy(const std::string& rawToken) {
  static constexpr const char* kSql = "DELETE FROM sessions WHERE token_hash = ?1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  bindText(stmt, 1, sha256Hex(rawToken));
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void SessionStore::destroyAllExcept(int64_t userId, const std::string& keepRawToken) {
  static constexpr const char* kSql =
      "DELETE FROM sessions WHERE user_id = ?1 AND token_hash != ?2;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  sqlite3_bind_int64(stmt, 1, userId);
  bindText(stmt, 2, sha256Hex(keepRawToken));
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void SessionStore::pruneExpired() {
  static constexpr const char* kSql = "DELETE FROM sessions WHERE expires_at <= ?1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return;  // best-effort cleanup; not worth failing the caller over
  }
  bindText(stmt, 1, wikicore::util::nowIso8601());
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

}  // namespace wikicore::auth
