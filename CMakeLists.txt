cmake_minimum_required(VERSION 3.21)
project(personal_wiki CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Prefer static linking of our own code + light dependencies; system libc/libstdc++/
# OpenSSL/zlib stay dynamic (see docs/architecture.md, "Build & Deployment").
add_compile_options(-Wall -Wextra)

find_package(SQLite3 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)
# Otherwise only ever pulled in transitively via Drogon (wiki-server) — an
# explicit find_package here is needed for unit_tests to link
# auth/Crypto.cpp (SHA-256, via EVP) directly without dragging in Drogon.
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------------------
# Semantic search (embeddings) — build-time-optional, independent of each
# other. Both default OFF: an SBC build stays exactly as light as today's
# until someone opts in. See docs/embeddings.md for the full rationale
# (why two provider kinds, the fail-loudly runtime contract, and the real
# zig/arm-musl -march= bug found and worked around below when spiking
# llama.cpp against cross/arm-musl before committing to it).
# ---------------------------------------------------------------------------
option(WIKI_ENABLE_LOCAL_EMBEDDINGS
       "Build the local (llama.cpp-backed) embedding provider — see docs/embeddings.md"
       OFF)
option(WIKI_ENABLE_CLOUD_EMBEDDINGS
       "Build the cloud (HTTP API) embedding provider — see docs/embeddings.md"
       OFF)

# ---------------------------------------------------------------------------
# libwikicore — config, vault, index/search, MCP tool logic. NO Drogon/
# OpenSSL/auth dependency: this is what wiki-mcp links against so the MCP
# stdio binary stays a thin, fast-starting process independent of the HTTP
# stack (and MCP tools are read-only in the MVP — no session/auth needed).
# ---------------------------------------------------------------------------
add_library(wikicore STATIC
  src/core/placeholder.cpp
  src/config/AppConfig.cpp
  src/vault/PathGuard.cpp
  src/vault/FrontMatter.cpp
  src/vault/VaultRepository.cpp
  src/vault/DocumentService.cpp
  src/vault/AttachmentService.cpp
  src/vault/AttachToDocument.cpp
  src/vault/McpUploadStaging.cpp
  src/vault/FolderService.cpp
  src/vault/BackupService.cpp
  src/index/Database.cpp
  src/index/IndexUpdater.cpp
  src/index/IndexBuilder.cpp
  src/index/FtsSearch.cpp
  src/index/McpAuditLog.cpp
  src/index/AgentChatStore.cpp
  src/index/GraphQueries.cpp
  src/index/NavQueries.cpp
  src/index/QueryBlocks.cpp
  src/index/SnapshotStore.cpp
  src/index/VaultWatcher.cpp
  src/util/Time.cpp
  src/util/Uuid.cpp
  src/util/Base64.cpp
  src/util/HtmlEscape.cpp
  src/util/Excerpt.cpp
  src/util/MarkdownRenderer.cpp
  src/util/WikiLinks.cpp
  src/util/YouTubeEmbed.cpp
  src/embeddings/NullEmbeddingProvider.cpp
  src/embeddings/EmbeddingProviderFactory.cpp
  src/embeddings/EmbeddingChunks.cpp
  src/llm/AgentRuntime.cpp
  src/llm/ChatStreamParser.cpp
)
target_include_directories(wikicore PUBLIC src)
target_link_libraries(wikicore PUBLIC
  SQLite3::SQLite3
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp
  md4c::md4c
  md4c::md4c-html
  tomlplusplus::tomlplusplus
  PkgConfig::ARGON2
)
# EmbeddingProviderFactory checks these at compile time to give the
# fail-loudly error from docs/embeddings.md ("Fail loudly, not silently")
# instead of silently falling back to NullEmbeddingProvider when a config
# asks for a provider kind this particular binary wasn't built with.
if(WIKI_ENABLE_LOCAL_EMBEDDINGS)
  target_compile_definitions(wikicore PUBLIC WIKI_ENABLE_LOCAL_EMBEDDINGS)
endif()
if(WIKI_ENABLE_CLOUD_EMBEDDINGS)
  target_compile_definitions(wikicore PUBLIC WIKI_ENABLE_CLOUD_EMBEDDINGS)
endif()

# ---------------------------------------------------------------------------
# wiki-server — HTTP entrypoint (Drogon). auth/ and controllers/ live here,
# not in libwikicore: they're a web-only concern (sessions, CSRF, argon2
# login) that wiki-mcp has no reason to link against.
# ---------------------------------------------------------------------------
find_package(Drogon CONFIG REQUIRED)  # also brings in drogon_create_views()

add_executable(wiki-server
  src/main.cpp
  src/auth/CidrMatch.cpp
  src/auth/ClientIp.cpp
  src/auth/Crypto.cpp
  src/auth/McpRemoteConfig.cpp
  src/auth/PasswordHasher.cpp
  src/auth/AdminAccount.cpp
  src/auth/SessionStore.cpp
  src/auth/RateLimiter.cpp
  src/auth/AuthServices.cpp
  src/auth/AuthFilter.cpp
  src/auth/CsrfFilter.cpp
  src/auth/RequireAdmin.cpp
  src/controllers/AuthRoutes.cpp
  src/controllers/DocumentRoutes.cpp
  src/controllers/SearchRoutes.cpp
  src/controllers/NavRoutes.cpp
  src/controllers/AdminRoutes.cpp
  src/controllers/FolderRoutes.cpp
  src/controllers/PageRoutes.cpp
  src/controllers/RemoteMcpRoutes.cpp
  src/controllers/VersionRoutes.cpp
  src/controllers/QueryRoutes.cpp
  src/controllers/GraphRoutes.cpp
  src/controllers/AgentRoutes.cpp
  src/llm/CloudChatClient.cpp
)
target_link_libraries(wiki-server PRIVATE wikicore Drogon::Drogon PkgConfig::ARGON2 OpenSSL::SSL OpenSSL::Crypto)

# No CSP views (views/*.csp) anymore — the backend is a JSON API only now;
# static/shell.html + client-side JS own all rendering (see
# docs/architecture.md's frontend section and PageRoutes.cpp). This used
# to drogon_create_views() two .csp templates into generated C++; removed
# along with the templates themselves.

# ---------------------------------------------------------------------------
# wiki-mcp — MCP stdio entrypoint. Deliberately NOT linked against Drogon
# (see the wikicore comment above) — read-only MCP tools have no reason to
# carry the HTTP stack, and MCP clients spawn this per-session so it needs
# to start fast.
#
# MCP protocol layer: hkr04/cpp-mcp, vendored via FetchContent and pinned
# to a commit (no vcpkg port exists for it). Spiked clean against this
# exact toolchain (GCC 16.2.1, C++20) before committing to it over the
# plan's hand-rolled-JSON-RPC fallback — see docs/architecture.md. Its own
# CMakeLists.txt builds its examples/ too (no option to skip); accepted as
# a few extra seconds of build time rather than fighting its build system's
# internals to cherry-pick just the `mcp` library target.
# ---------------------------------------------------------------------------
include(FetchContent)
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  cpp_mcp
  GIT_REPOSITORY https://github.com/hkr04/cpp-mcp.git
  GIT_TAG 13a8bef5f1885b10b8d6038c4b319b7024744fa5
)
FetchContent_MakeAvailable(cpp_mcp)

# CloudChatClient.cpp (wiki-server) includes the same vendored httplib.h
# cpp-mcp already fetched — same pattern CloudEmbeddingProvider uses,
# just always-on because the Draft agent is part of the HTTP binary, not
# behind WIKI_ENABLE_CLOUD_EMBEDDINGS.
target_include_directories(wiki-server PRIVATE ${cpp_mcp_SOURCE_DIR}/common)

add_executable(wiki-mcp
  src/mcp_main.cpp
  src/mcp/McpServer.cpp
)
target_link_libraries(wiki-mcp PRIVATE wikicore mcp)
# cpp-mcp's own CMakeLists.txt sets these up via directory-scoped
# include_directories() rather than target_include_directories() on the
# `mcp` target, so they don't propagate to us automatically — add them
# explicitly.
target_include_directories(wiki-mcp PRIVATE ${cpp_mcp_SOURCE_DIR}/include
                                              ${cpp_mcp_SOURCE_DIR}/common)

# ---------------------------------------------------------------------------
# sqlite-vec (document_embeddings storage) — only needed when at least one
# embedding provider is compiled in; a build with neither has nothing to
# store vectors for. FetchContent + pinned commit (same discipline as
# cpp_mcp/llama_cpp above; no vcpkg port exists).
#
# sqlite-vec.h is normally generated from sqlite-vec.h.tmpl by upstream's
# own Makefile shelling out to `envsubst`. Reproduced here with plain
# CMake string(REPLACE) instead of requiring envsubst on the build host —
# every substituted value is version/date/source METADATA this project
# never reads (nothing here calls SQLITE_VEC_VERSION or similar), so
# there's no reason to add a new build-host tool dependency just to fill
# in values nothing uses.
# ---------------------------------------------------------------------------
if(WIKI_ENABLE_LOCAL_EMBEDDINGS OR WIKI_ENABLE_CLOUD_EMBEDDINGS)
  # sqlite-vec.c is plain C, and this project's own project() call at the
  # top of this file only enables CXX (llama.cpp's own subproject enables
  # C for ITS sources via its own project() call, but that doesn't cover
  # a .c file compiled directly into wikicore's sources the way this one
  # is) — enable_language(C) here, not globally at the top of this file,
  # so a build with both embedding flags off never needs a C compiler at
  # all, matching this project's existing pure-C++ baseline.
  enable_language(C)

  FetchContent_Declare(
    sqlite_vec
    GIT_REPOSITORY https://github.com/asg017/sqlite-vec.git
    GIT_TAG 04d28bd21773981e2d266bbf6aa4efbd011eb4f6
  )
  FetchContent_Populate(sqlite_vec)

  file(READ "${sqlite_vec_SOURCE_DIR}/sqlite-vec.h.tmpl" _wiki_sqlite_vec_h)
  string(REPLACE "\${VERSION_MAJOR}" "0" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  string(REPLACE "\${VERSION_MINOR}" "1" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  string(REPLACE "\${VERSION_PATCH}" "10" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  string(REPLACE "\${VERSION}" "0.1.10-alpha.4" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  string(REPLACE "\${DATE}" "unknown" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  string(REPLACE "\${SOURCE}" "${sqlite_vec_SOURCE_DIR}" _wiki_sqlite_vec_h "${_wiki_sqlite_vec_h}")
  file(WRITE "${CMAKE_BINARY_DIR}/generated/sqlite-vec.h" "${_wiki_sqlite_vec_h}")
  unset(_wiki_sqlite_vec_h)

  target_sources(wikicore PRIVATE
    src/index/EmbeddingIndexer.cpp
    src/index/EmbeddingsRuntimeConfig.cpp
    "${sqlite_vec_SOURCE_DIR}/sqlite-vec.c"
  )
  target_include_directories(wikicore PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    "${sqlite_vec_SOURCE_DIR}"
  )
  target_compile_definitions(wikicore PUBLIC WIKI_ENABLE_SQLITE_VEC)
  # SQLITE_VEC_STATIC: this project compiles sqlite-vec.c straight into
  # wikicore rather than loading it as a separate .so via
  # sqlite3_load_extension — that macro just keeps SQLITE_VEC_API from
  # trying to __declspec(dllexport) symbols that are never exported as a
  # shared library here (a Windows-only concern; harmless no-op on Linux,
  # correct either way).
  set_source_files_properties("${sqlite_vec_SOURCE_DIR}/sqlite-vec.c" PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_VEC_STATIC"
  )
endif()

# ---------------------------------------------------------------------------
# Cloud embedding provider — no new dependency at all: reuses cpp-mcp's own
# vendored cpp-httplib (${cpp_mcp_SOURCE_DIR}/common/httplib.h), the exact
# same header, already fetched above for wiki-mcp. cpp-mcp itself builds
# with MCP_SSL/CPPHTTPLIB_OPENSSL_SUPPORT off (it doesn't need outbound
# HTTPS), but httplib.h is header-only — CloudEmbeddingProvider.cpp defines
# CPPHTTPLIB_OPENSSL_SUPPORT itself before including it, affecting only
# that one translation unit, and links against the OpenSSL this project
# already requires (find_package(OpenSSL REQUIRED) near the top of this
# file) for the outbound HTTPS an external embeddings API needs.
# ---------------------------------------------------------------------------
if(WIKI_ENABLE_CLOUD_EMBEDDINGS)
  target_sources(wikicore PRIVATE src/embeddings/CloudEmbeddingProvider.cpp)
  target_include_directories(wikicore PRIVATE ${cpp_mcp_SOURCE_DIR}/common)
  target_link_libraries(wikicore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# ---------------------------------------------------------------------------
# Local embedding provider — llama.cpp, FetchContent + pinned commit (same
# discipline as cpp_mcp above; no vcpkg port exists). See
# docs/embeddings.md's "The real zig/arm-musl bug found cross-compiling
# llama.cpp" section for what the CMAKE_C_FLAGS/CMAKE_CXX_FLAGS override
# below actually works around — short version: zig 0.16.0's ARM -march=
# parser rejects any LLVM-standard "-march=<arch>-a" string outright, even
# with zero extensions, and ggml's own -march= is exactly that spelling.
# Leaving GGML_CPU_ARM_ARCH unset sidesteps the broken code path (ggml
# then falls back to whatever -mcpu the toolchain wrapper already
# supplies), but that alone under-builds one file that needs ARM NEON
# FP16 intrinsics unavailable at the wrapper's default -mcpu=generic+v7a
# baseline — passing +fp16 a second time via CMAKE_C_FLAGS/CMAKE_CXX_FLAGS
# fixes it (clang honors the LAST -mcpu on the command line). Scoped to
# only this FetchContent_MakeAvailable call, restored immediately after —
# see the comment on why this must NOT be a global toolchain change.
# ---------------------------------------------------------------------------
if(WIKI_ENABLE_LOCAL_EMBEDDINGS)
  set(_wiki_saved_c_flags "${CMAKE_C_FLAGS}")
  set(_wiki_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
  if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=generic+v7a+fp16")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=generic+v7a+fp16")
  endif()

  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  # NOT unconditionally OFF — confirmed live (SIGILL, "Illegal
  # instruction") that a hardcoded OFF here makes ggml pick one fixed CPU
  # backend variant (AVX2 among its flags) regardless of what the actual
  # build host supports; this machine's own CPU has AVX/FMA/F16C but NOT
  # AVX2, and the resulting binary crashed on its very first embed() call.
  # OFF is still correct, and necessary, for cross-compiling (ggml must
  # never probe the BUILD host's CPU when the RUN target is a different
  # architecture entirely) — ON for a native build lets ggml detect this
  # exact host's real feature set instead of guessing a fixed one.
  if(CMAKE_CROSSCOMPILING)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  else()
    set(GGML_NATIVE ON CACHE BOOL "" FORCE)
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG 2b1847030cef76ef315eaee0b7ae0cdcd4fb15ff
  )
  FetchContent_MakeAvailable(llama_cpp)

  set(CMAKE_C_FLAGS "${_wiki_saved_c_flags}")
  set(CMAKE_CXX_FLAGS "${_wiki_saved_cxx_flags}")
  unset(_wiki_saved_c_flags)
  unset(_wiki_saved_cxx_flags)

  # Added to wikicore here (target_sources on the already-declared target),
  # not in its own add_library() block near the top of this file, because
  # the `llama` target this depends on doesn't exist until the
  # FetchContent_MakeAvailable() call just above.
  target_sources(wikicore PRIVATE src/embeddings/LocalEmbeddingProvider.cpp)
  target_include_directories(wikicore PRIVATE
    ${llama_cpp_SOURCE_DIR}/include
    ${llama_cpp_SOURCE_DIR}/ggml/include
  )
  target_link_libraries(wikicore PRIVATE llama)
endif()

# ---------------------------------------------------------------------------
# Install layout — matches what systemd/wiki.service expects
# (WorkingDirectory=/opt/wiki, static/ resolved relative to cwd by
# app().setDocumentRoot("static") in main.cpp). Point
# -DCMAKE_INSTALL_PREFIX at wherever this actually gets deployed (e.g.
# /opt/wiki); it's deliberately not hardcoded here. config.toml itself is
# NOT installed — only the .example is, so a real one is never silently
# materialized from defaults; see docs/deployment.md.
#
# This is plain `cmake --install`, not a CPack .deb/tarball: the M5-
# approved deployment model is a native build directly on the target
# device (cross-compilation is Phase 2), so there's no distributable
# artifact to package yet — just an install tree ready for systemd.
# ---------------------------------------------------------------------------
install(TARGETS wiki-server wiki-mcp RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION static)
install(FILES config.example.toml DESTINATION .)
install(FILES systemd/wiki.service systemd/wiki.env.example DESTINATION share/wiki/systemd)
# wiki-backup.sh is a PROGRAMS install (not FILES) so it lands executable —
# wiki-backup.service's ExecStart= points straight at /opt/wiki/bin/wiki-backup.sh,
# matching where wiki-server/wiki-mcp themselves land above.
install(PROGRAMS systemd/wiki-backup.sh DESTINATION bin)
install(FILES systemd/wiki-backup.service systemd/wiki-backup.timer
              systemd/wiki-backup.env.example DESTINATION share/wiki/systemd)

option(WIKI_ENABLE_FUZZING
       "Build libFuzzer targets under tests/fuzz/ (requires Clang; see that dir's CMakeLists.txt)"
       OFF)

enable_testing()
add_subdirectory(tests)
if(WIKI_ENABLE_FUZZING)
  add_subdirectory(tests/fuzz)
endif()
