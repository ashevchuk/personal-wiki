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
  src/index/Database.cpp
  src/index/IndexUpdater.cpp
  src/index/IndexBuilder.cpp
  src/index/FtsSearch.cpp
  src/index/NavQueries.cpp
  src/index/VaultWatcher.cpp
  src/util/Time.cpp
  src/util/Uuid.cpp
  src/util/HtmlEscape.cpp
  src/util/Excerpt.cpp
  src/util/MarkdownRenderer.cpp
  src/util/BasePath.cpp
  src/util/PageChrome.cpp
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

# ---------------------------------------------------------------------------
# wiki-server — HTTP entrypoint (Drogon). auth/ and controllers/ live here,
# not in libwikicore: they're a web-only concern (sessions, CSRF, argon2
# login) that wiki-mcp has no reason to link against.
# ---------------------------------------------------------------------------
find_package(Drogon CONFIG REQUIRED)  # also brings in drogon_create_views()

add_executable(wiki-server
  src/main.cpp
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
)
target_link_libraries(wiki-server PRIVATE wikicore Drogon::Drogon PkgConfig::ARGON2)

# CSP views (views/*.csp) compiled to C++ by drogon_ctl. Generated classes
# are deliberately left in the global namespace (no -n/path-to-namespace)
# so the plain filename ("EditPage") is both the .csp basename AND the
# exact string newHttpViewResponse()/DrClassMap look it up by — we already
# got the fully-qualified-name gotcha wrong once for HttpFilter, see
# docs/architecture.md; same underlying DrObject<T> mechanism applies here.
set(GENERATED_VIEWS_DIR ${CMAKE_BINARY_DIR}/generated_views)
drogon_create_views(wiki-server ${CMAKE_CURRENT_SOURCE_DIR}/views ${GENERATED_VIEWS_DIR})
target_include_directories(wiki-server PRIVATE ${GENERATED_VIEWS_DIR})

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

enable_testing()
add_subdirectory(tests)
