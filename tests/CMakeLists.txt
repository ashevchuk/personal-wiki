find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
  unit/PathGuardTest.cpp
  unit/FrontMatterTest.cpp
  unit/PasswordHasherTest.cpp
  unit/RateLimiterTest.cpp
  # PasswordHasher/RateLimiter are auth/-only (see root CMakeLists.txt
  # comment on why auth/ isn't part of wikicore) but have zero Drogon/
  # OpenSSL dependency themselves, so it's cheap to compile them straight
  # into the test binary rather than standing up a whole second library.
  ${CMAKE_SOURCE_DIR}/src/auth/PasswordHasher.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/RateLimiter.cpp
)
target_link_libraries(unit_tests PRIVATE wikicore Catch2::Catch2WithMain)

add_test(NAME unit_tests COMMAND unit_tests)
