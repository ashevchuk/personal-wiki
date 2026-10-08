# Cross-compile toolchain: generic AArch64 (Cortex-A53-class, e.g. the
# Allwinner H6 in an Orange Pi One Plus), musl libc, fully static — via
# zig cc/c++ (bundled libc/libc++, no separate toolchain download). Same
# shape as ../arm-musl/ (that one targets 32-bit ARMv7); this one exists
# because the actual test target runs an arm64 kernel with
# CONFIG_COMPAT unset — a 32-bit ARMv7 binary doesn't just run slower
# there, the kernel refuses to exec it at all (ENOEXEC).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/cc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/c++)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/ranlib CACHE FILEPATH "")

set(CMAKE_C_COMPILER_TARGET aarch64-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-musl)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Carried over from ../arm-musl/toolchain.cmake, UNVERIFIED on this
# architecture: the zig/lld depfile SIGSEGV workaround was found cross-
# linking for arm-linux-musleabihf (32-bit ARM), plausibly tied to that
# backend specifically rather than generic to zig's cross-linking path —
# not confirmed one way or the other until this target actually gets
# built. Harmless to keep if unneeded; if the build links cleanly
# without it, drop it.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

# Same C++20 module-scanning workaround as ../arm-musl/ — not
# architecture-specific (clang-scan-deps can't resolve zig's bundled
# libc++ for ANY foreign --target), kept unconditionally.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
