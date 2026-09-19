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
  unit/EmbeddingProviderFactoryTest.cpp
  # PasswordHasher/RateLimiter are auth/-only (see root CMakeLists.txt
  # comment on why auth/ isn't part of wikicore) but have zero Drogon/
  # OpenSSL dependency themselves, so it's cheap to compile them straight
  # into the test binary rather than standing up a whole second library.
  unit/CidrMatchTest.cpp
  unit/McpRemoteConfigTest.cpp
  unit/CryptoTest.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/PasswordHasher.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/RateLimiter.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/CidrMatch.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/Crypto.cpp
  ${CMAKE_SOURCE_DIR}/src/auth/McpRemoteConfig.cpp
)
if(WIKI_ENABLE_LOCAL_EMBEDDINGS OR WIKI_ENABLE_CLOUD_EMBEDDINGS)
  # EmbeddingIndexer/EmbeddingsRuntimeConfig only exist in wikicore when
  # sqlite-vec was linked in (see root CMakeLists.txt's "sqlite-vec"
  # section) — same condition.
  target_sources(unit_tests PRIVATE
    unit/EmbeddingIndexerTest.cpp
    unit/EmbeddingsRuntimeConfigTest.cpp
  )
endif()

target_link_libraries(unit_tests PRIVATE wikicore Catch2::Catch2WithMain OpenSSL::Crypto)

add_test(NAME unit_tests COMMAND unit_tests)

# Real-model semantic-similarity proof for LocalEmbeddingProvider — NOT run
# by default, and deliberately opt-in via a real GGUF file path rather than
# committed to the repo (models are tens of MB, and downloading one on every
# CI run for a test that exists to sanity-check one provider implementation
# isn't worth the cost). Registered only when BOTH
# WIKI_ENABLE_LOCAL_EMBEDDINGS is on (the provider must actually be
# compiled in) AND WIKI_TEST_EMBEDDING_MODEL_PATH points at a real file —
# same opt-in shape as security_e2e.py's "only if wiki-server exists" gate
# above. See docs/embeddings.md's "Real-model verification" section for the
# actual cosine-similarity numbers this test's thresholds are based on
# (observed with bge-small-en-v1.5, not guessed).
set(WIKI_TEST_EMBEDDING_MODEL_PATH "" CACHE FILEPATH
    "Path to a real GGUF embedding model, to run LocalEmbeddingProviderTest against it")
if(WIKI_ENABLE_LOCAL_EMBEDDINGS AND WIKI_TEST_EMBEDDING_MODEL_PATH)
  if(NOT EXISTS "${WIKI_TEST_EMBEDDING_MODEL_PATH}")
    message(FATAL_ERROR "WIKI_TEST_EMBEDDING_MODEL_PATH is set to "
            "'${WIKI_TEST_EMBEDDING_MODEL_PATH}' but that file doesn't exist")
  endif()
  add_executable(local_embedding_provider_test
    unit/LocalEmbeddingProviderTest.cpp
  )
  target_link_libraries(local_embedding_provider_test PRIVATE wikicore Catch2::Catch2WithMain)
  target_compile_definitions(local_embedding_provider_test PRIVATE
    WIKI_TEST_EMBEDDING_MODEL_PATH="${WIKI_TEST_EMBEDDING_MODEL_PATH}"
  )
  add_test(NAME local_embedding_provider_test COMMAND local_embedding_provider_test)
  # Loading a real model + running inference is much slower than this
  # project's other unit tests (milliseconds each) — same reasoning as the
  # stress tests' own TIMEOUT below, not a sign anything is wrong.
  set_tests_properties(local_embedding_provider_test PROPERTIES TIMEOUT 60 LABELS real-model)

  # Real end-to-end proof of IndexUpdater's embedding-on-write wiring (main
  # CMakeLists.txt's "sqlite-vec"/IndexUpdater.h changes) — same real-model
  # gate as local_embedding_provider_test above, since it needs the exact
  # same GGUF file.
  add_executable(index_updater_embedding_test
    unit/IndexUpdaterEmbeddingTest.cpp
  )
  target_link_libraries(index_updater_embedding_test PRIVATE wikicore Catch2::Catch2WithMain)
  target_compile_definitions(index_updater_embedding_test PRIVATE
    WIKI_TEST_EMBEDDING_MODEL_PATH="${WIKI_TEST_EMBEDDING_MODEL_PATH}"
  )
  add_test(NAME index_updater_embedding_test COMMAND index_updater_embedding_test)
  set_tests_properties(index_updater_embedding_test PROPERTIES TIMEOUT 60 LABELS real-model)

  # Real end-to-end proof of FtsSearch's hybrid (BM25 + semantic) ranking
  # — same real-model gate, needs the same GGUF file.
  add_executable(fts_search_hybrid_test
    unit/FtsSearchHybridTest.cpp
  )
  target_link_libraries(fts_search_hybrid_test PRIVATE wikicore Catch2::Catch2WithMain)
  target_compile_definitions(fts_search_hybrid_test PRIVATE
    WIKI_TEST_EMBEDDING_MODEL_PATH="${WIKI_TEST_EMBEDDING_MODEL_PATH}"
  )
  add_test(NAME fts_search_hybrid_test COMMAND fts_search_hybrid_test)
  set_tests_properties(fts_search_hybrid_test PROPERTIES TIMEOUT 60 LABELS real-model)
endif()

# CloudEmbeddingProvider's real-API test: gated on the build flag only —
# whether it actually RUNS (vs. SKIPs) is a runtime decision inside the
# test itself, based on whether OPENAI_API_KEY is set in the environment
# at ctest time (a real secret, never something CMake configure should
# require or bake in). See docs/embeddings.md.
if(WIKI_ENABLE_CLOUD_EMBEDDINGS)
  add_executable(cloud_embedding_provider_test
    unit/CloudEmbeddingProviderTest.cpp
  )
  target_link_libraries(cloud_embedding_provider_test PRIVATE wikicore Catch2::Catch2WithMain)
  add_test(NAME cloud_embedding_provider_test COMMAND cloud_embedding_provider_test)
  # Real network call to a real external API — slower and less reliable
  # than this project's other tests by nature, not a sign anything local
  # is wrong.
  # SKIP_RETURN_CODE 4: Catch2's own exit code for a SKIP() — without this,
  # ctest has no way to know that exit code means "intentionally skipped"
  # rather than "failed", and reports a clean, expected skip (no
  # OPENAI_API_KEY in the environment) as a red failure.
  set_tests_properties(cloud_embedding_provider_test PROPERTIES
    TIMEOUT 30 LABELS real-api SKIP_RETURN_CODE 4)
endif()

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

  # Same gate as fts_search_hybrid_test / local_embedding_provider_test
  # above: needs a real WIKI_ENABLE_LOCAL_EMBEDDINGS build AND a real GGUF
  # model file — drives real concurrent embed() calls through a real
  # wiki-server process (see stress_embeddings_concurrency.py's own module
  # docstring for exactly what this proves that the isolated
  # LocalEmbeddingProviderTest.cpp concurrency test doesn't).
  if(WIKI_ENABLE_LOCAL_EMBEDDINGS AND WIKI_TEST_EMBEDDING_MODEL_PATH)
    add_test(
      NAME stress_embeddings_concurrency
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/integration/stress_embeddings_concurrency.py
              $<TARGET_FILE:wiki-server> "${WIKI_TEST_EMBEDDING_MODEL_PATH}"
    )
    set_tests_properties(stress_embeddings_concurrency PROPERTIES
      TIMEOUT 120 LABELS "stress;real-model")
  endif()
endif()
