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
