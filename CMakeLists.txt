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
  src/util/Time.cpp
  src/util/Uuid.cpp
  src/util/HtmlEscape.cpp
  src/util/Excerpt.cpp
  src/util/MarkdownRenderer.cpp
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
# wiki-mcp — MCP stdio entrypoint. Deliberately NOT linked against Drogon.
# Implementation lands in Milestone 4; placeholder keeps the two-binary
# layout real from day one so nothing has to be re-architected later.
# ---------------------------------------------------------------------------
add_executable(wiki-mcp
  src/mcp_main.cpp
)
target_link_libraries(wiki-mcp PRIVATE wikicore)

enable_testing()
add_subdirectory(tests)
