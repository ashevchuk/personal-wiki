#include "vault/SkillDocuments.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::vault;

TEST_CASE("isSkillType", "[SkillDocuments]") {
  REQUIRE(isSkillType("skill"));
  REQUIRE(isSkillType("Skill"));
  REQUIRE(isSkillType(" SKILL\t"));
  REQUIRE_FALSE(isSkillType(""));
  REQUIRE_FALSE(isSkillType("note"));
  REQUIRE_FALSE(isSkillType("skills"));
  REQUIRE_FALSE(isSkillType("skil"));
  REQUIRE_FALSE(isSkillType("my skill"));
}

TEST_CASE("rejectSkillType throws only for skill types", "[SkillDocuments]") {
  REQUIRE_THROWS_AS(rejectSkillType("skill"), SkillDocumentReadOnlyError);
  REQUIRE_THROWS_AS(rejectSkillType("Skill"), SkillDocumentReadOnlyError);
  REQUIRE_NOTHROW(rejectSkillType("note"));
  REQUIRE_NOTHROW(rejectSkillType(""));
}
