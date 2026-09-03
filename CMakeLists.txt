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
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)

# ---------------------------------------------------------------------------
# libwikicore — vault, index/search, MCP tool logic. NO Drogon/OpenSSL
# dependency: this is what wiki-mcp links against so the MCP stdio binary
# stays a thin, fast-starting process independent of the HTTP stack.
# ---------------------------------------------------------------------------
add_library(wikicore STATIC
  src/core/placeholder.cpp
  src/vault/PathGuard.cpp
)
target_include_directories(wikicore PUBLIC src)
target_link_libraries(wikicore PUBLIC
  SQLite3::SQLite3
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp
  md4c::md4c
  PkgConfig::ARGON2
)

# ---------------------------------------------------------------------------
# wiki-server — HTTP entrypoint (Drogon).
# ---------------------------------------------------------------------------
find_package(Drogon CONFIG REQUIRED)

add_executable(wiki-server
  src/main.cpp
)
target_link_libraries(wiki-server PRIVATE wikicore Drogon::Drogon)

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
