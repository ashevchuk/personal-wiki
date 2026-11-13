cmake_minimum_required(VERSION 3.21)
project(wiki-tray VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(raylib CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
# sdbus-c++'s own CMake package config exports a bare "-lsdbus-c++" with no
# absolute path, which the linker can't resolve against vcpkg's install
# tree — its .pc file (which carries the real -L path) works correctly, so
# go through PkgConfig here instead, same as X11 above.
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)

include(FetchContent)
# Independent copy of the same technique the wiki repo itself uses to get
# httplib.h header-only — this project's build has no dependency on the
# wiki's own FetchContent result.
FetchContent_Declare(
  cpp_mcp
  GIT_REPOSITORY https://github.com/hkr04/cpp-mcp.git
  GIT_TAG 13a8bef5f1885b10b8d6038c4b319b7024744fa5
)
FetchContent_Populate(cpp_mcp)

add_executable(wiki-tray
  src/main.cpp
  src/Config.cpp
  src/AgentClient.cpp
  src/TrayIcon.cpp
  src/GlobalHotkey.cpp
  src/ChatWindow.cpp
  src/WindowRaise.cpp
  src/IdleWait.cpp
)

# third_party/raygui.h shadows vcpkg's own copy (listed first, so it's
# found before raylib's include dir when wiki-tray's own sources write
# `#include <raygui.h>`) — it's the same raygui 4.0 with a two-line fix
# for a real upstream bug in GuiTextBox's multi-byte-UTF-8 backspace/
# delete handling. See third_party/raygui.h's own top comment.
target_include_directories(wiki-tray PRIVATE
  third_party
  ${cpp_mcp_SOURCE_DIR}/common
)
target_link_libraries(wiki-tray PRIVATE
  raylib
  # raylib's own vcpkg-exported CMake target doesn't declare glfw as a
  # link dependency (confirmed heuristically-generated and incomplete,
  # per vcpkg's own install-time note) — link it explicitly or any TU
  # that actually calls raylib functions fails at link time with
  # undefined glfw* references.
  glfw
  PkgConfig::SDBUSCPP
  PkgConfig::X11
  nlohmann_json::nlohmann_json
  tomlplusplus::tomlplusplus
  OpenSSL::SSL
  OpenSSL::Crypto
)
target_compile_definitions(wiki-tray PRIVATE
  WIKI_TRAY_VERSION="${PROJECT_VERSION}"
  WIKI_TRAY_SOURCE_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)
