# Cross-compile toolchain: generic x86_64 (baseline, no AVX2/etc. assumed),
# musl libc, fully static — via zig cc/c++ (bundled libc/libc++, no separate
# toolchain download, no dependency on the build host's own libc version).
#
# Why musl + static for x86_64 too, not just the ARM targets: the goal is a
# single binary that runs on any x86_64 Linux regardless of its glibc
# version, the same "doesn't touch the target's libc at all" property
# arm-musl/armv6-musl/aarch64-musl already have. See docs/sbc-deployment.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/cc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/c++)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/ranlib CACHE FILEPATH "")

set(CMAKE_C_COMPILER_TARGET x86_64-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-musl)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Carried over from cross/arm-musl/toolchain.cmake's zig/lld depfile-SIGSEGV
# workaround — UNVERIFIED for this target specifically; the crash was
# bisected on arm-linux-musleabihf, not x86_64-linux-musl. Left on as a
# precaution until a real build proves it unnecessary here.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# Same clang-scan-deps/zig-bundled-libc++ mismatch as the ARM toolchains —
# nothing here needs C++20 modules, so just turn scanning off.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
