#include "auth/AdminAccount.h"

#include "util/Time.h"

#include <sqlite3.h>

#include <stdexcept>

namespace wikicore::auth {

namespace {

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

void AdminAccount::createOrReplace(const std::string& username,
                                    const std::string& encodedPasswordHash) {
  static constexpr const char* kSql =
      "INSERT INTO users(id, username, password_hash, created_at) "
      "VALUES (1, ?1, ?2, ?3) "
      "ON CONFLICT(id) DO UPDATE SET "
      "  username = excluded.username, "
      "  password_hash = excluded.password_hash;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }
  bindText(stmt, 1, username);
  bindText(stmt, 2, encodedPasswordHash);
  bindText(stmt, 3, wikicore::util::nowIso8601());

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("failed to write admin account: ") +
                              sqlite3_errmsg(db_.handle()));
  }
}

std::optional<AdminUser> AdminAccount::find() const {
  static constexpr const char* kSql =
      "SELECT id, username, password_hash FROM users WHERE id = 1;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") +
                              sqlite3_errmsg(db_.handle()));
  }

  std::optional<AdminUser> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    AdminUser user;
    user.id = sqlite3_column_int64(stmt, 0);
    user.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    user.passwordHash =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    result = std::move(user);
  }
  sqlite3_finalize(stmt);
  return result;
}

}  // namespace wikicore::auth
