#pragma once

#include "index/Database.h"

#include <optional>
#include <string>

namespace wikicore::auth {

struct AdminUser {
  int64_t id;
  std::string username;
  std::string passwordHash;  // argon2id-encoded, see PasswordHasher
};

// The `users` table holds exactly one row (id=1, enforced by a CHECK
// constraint) — there is no multi-user support, by design (see the plan).
// This is the CRUD surface for that single row: `wiki-server
// --create-admin` writes it, AuthController's login handler reads it.
class AdminAccount {
 public:
  explicit AdminAccount(index::Database& db) : db_(db) {}

  // Creates the admin row if none exists, or overwrites username/password
  // if one does (re-running --create-admin is how you reset credentials).
  void createOrReplace(const std::string& username,
                        const std::string& encodedPasswordHash);

  // nullopt if no admin has been created yet.
  std::optional<AdminUser> find() const;

 private:
  index::Database& db_;
};

}  // namespace wikicore::auth
