find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
  unit/PathGuardTest.cpp
  unit/FrontMatterTest.cpp
  unit/AppConfigTest.cpp
  unit/PasswordHasherTest.cpp
  unit/RateLimiterTest.cpp
  unit/DocumentServiceTest.cpp
  unit/AttachmentServiceTest.cpp
  unit/FolderServiceTest.cpp
  unit/FtsSearchTest.cpp
  unit/NavQueriesTest.cpp
  unit/IndexBuilderTest.cpp
  unit/VaultWatcherTest.cpp
  unit/WikiLinksTest.cpp
  unit/YouTubeEmbedTest.cpp
  unit/MarkdownRendererTest.cpp
  unit/SnapshotStoreTest.cpp
  unit/McpAuditLogTest.cpp
  unit/BackupServiceTest.cpp
  # PasswordHasher/RateLimiter are auth/-only (see root CMakeLists.txt
  # comment on why auth/ isn't part of wikicore) but have zero Drogon/
  # OpenSSL dependency themselves, so it's cheap to compile them straight
  # into the test binary rather than standing up a whole second library.
  unit/CidrMatchTest.cpp
  unit/McpRemoteConfigTest.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/PasswordHasher.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/RateLimiter.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/CidrMatch.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/Crypto.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/McpRemoteConfig.cpp
)
target_link_libraries(unit_tests PRIVATE wikicore Catch2::Catch2WithMain OpenSSL::Crypto)

add_test(NAME unit_tests COMMAND unit_tests)

# HTTP-layer security/correctness checks (auth, CSRF, path traversal,
# session fixation, visibility gating, rate limiting, VaultWatcher) against
# a real wiki-server process — see tests/integration/security_e2e.py's
# module docstring for why this exists as a committed, ctest-run script
# and not just an ad hoc shell session (M2 postmortem, docs/architecture.md).
# Only registered if wiki-server actually got configured (it depends on
# Drogon and the vendored frontend assets; the plain `unit_tests` target
# above has none of those dependencies).
if(TARGET wiki-server)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME security_e2e
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/security_e2e.py
            $<TARGET_FILE:wiki-server>
  )
  set_tests_properties(security_e2e PROPERTIES TIMEOUT 60)

  # Stress tests: concurrency races and resource-exhaustion behavior, NOT
  # logical correctness (that's security_e2e.py above) — see
  # tests/integration/_stress_common.py's module docstring. Slower and
  # partly informational (some of what they print is a measured, accepted
  # gap rather than a hard failure), so they carry a "stress" LABEL and
  # are excluded from the label-filtered default sweep a CI job would
  # normally run (`ctest -LE stress` for the fast path; plain `ctest` or
  # `ctest -L stress` to include them). Same wiki-server dependency gate
  # as security_e2e.py.
  add_test(
    NAME stress_concurrency
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/stress_concurrency.py
            $<TARGET_FILE:wiki-server>
  )
  set_tests_properties(stress_concurrency PROPERTIES TIMEOUT 120 LABELS stress)

  add_test(
    NAME stress_resources
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/stress_resources.py
            $<TARGET_FILE:wiki-server>
  )
  set_tests_properties(stress_resources PROPERTIES TIMEOUT 180 LABELS stress)
endif()
