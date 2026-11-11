#include "auth/PasswordHasher.h"

#include <catch2/catch_test_macros.hpp>

using wikicore::auth::PasswordHasher;

TEST_CASE("PasswordHasher verifies the correct password", "[PasswordHasher]") {
  const std::string hash = PasswordHasher::hash("SuperSecret123");
  REQUIRE(PasswordHasher::verify(hash, "SuperSecret123"));
}

TEST_CASE("PasswordHasher rejects a wrong password", "[PasswordHasher]") {
  const std::string hash = PasswordHasher::hash("SuperSecret123");
  REQUIRE_FALSE(PasswordHasher::verify(hash, "WrongPassword"));
}

TEST_CASE("PasswordHasher rejects an empty password against a real hash",
          "[PasswordHasher]") {
  const std::string hash = PasswordHasher::hash("SuperSecret123");
  REQUIRE_FALSE(PasswordHasher::verify(hash, ""));
}

TEST_CASE("PasswordHasher produces a different hash each time (random salt)",
          "[PasswordHasher]") {
  const std::string hashA = PasswordHasher::hash("SamePassword");
  const std::string hashB = PasswordHasher::hash("SamePassword");
  REQUIRE(hashA != hashB);
  REQUIRE(PasswordHasher::verify(hashA, "SamePassword"));
  REQUIRE(PasswordHasher::verify(hashB, "SamePassword"));
}

TEST_CASE("newPasswordProblem enforces the length bounds", "[PasswordHasher]") {
  using wikicore::auth::newPasswordProblem;
  REQUIRE_FALSE(newPasswordProblem("").empty());
  REQUIRE_FALSE(newPasswordProblem("elevenchars").empty());
  REQUIRE(newPasswordProblem("twelve chars").empty());
  // 12 Cyrillic characters, 24 bytes.
  REQUIRE(newPasswordProblem("парольпароль").empty());
  // 6 Cyrillic characters are 12 bytes but only 6 characters.
  REQUIRE_FALSE(newPasswordProblem("пароль").empty());
  REQUIRE(newPasswordProblem(std::string(1024, 'x')).empty());
  REQUIRE_FALSE(newPasswordProblem(std::string(1025, 'x')).empty());
}
