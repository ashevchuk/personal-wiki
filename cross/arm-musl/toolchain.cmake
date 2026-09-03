# Cross-compile toolchain: armv7 (Cortex-A7-class, hardfloat), musl libc,
# fully static — via zig cc/c++ (bundled libc/libc++, no separate
# toolchain download, no dependency on the build host's own libc version).
#
# Why musl + static, not a glibc cross-toolchain: the actual deployment
# target (Debian 9 stretch armhf, glibc 2.24 from 2016) is old enough that
# any modern glibc cross-toolchain would link against a NEWER glibc than
# what's on the target, and the binary would fail at runtime with
# "GLIBC_2.XX not found". A fully static musl binary sidesteps this
# entirely — it doesn't touch the target's libc at all. See
# docs/deployment.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/cc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/c++)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/ranlib CACHE FILEPATH "")

set(CMAKE_C_COMPILER_TARGET arm-linux-musleabihf)
set(CMAKE_CXX_COMPILER_TARGET arm-linux-musleabihf)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# CMake (>=3.20, Ninja generator) auto-adds "-Xlinker --dependency-file=..."
# to every link command for linker-level dependency tracking. Zig's bundled
# lld SEGFAULTS (code=139) when asked to emit a dependency file while
# statically linking for arm-linux-musleabihf — confirmed by bisecting a
# manually-reproduced link command flag-by-flag: identical command with this
# flag removed links cleanly every time, with it present it crashes 100% of
# the time, regardless of parallelism or archive count. Real bug in zig
# 0.16.0's lld for this target/flag combo, not our code. Just don't ask for
# the depfile — CMake falls back to non-linker-based dependency tracking.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# CMake/Ninja auto-enable C++20 module dependency scanning
# (clang-scan-deps -format=p1689) for Clang+C++20, regardless of whether
# the project actually uses modules (it doesn't — Drogon/trantor don't).
# The SYSTEM clang-scan-deps binary doesn't know how to resolve zig's
# bundled libc++ for a foreign --target, so it fails to find <string>/
# <vector>/etc. even though normal compilation finds them fine. Nothing
# here needs modules, so just turn scanning off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
