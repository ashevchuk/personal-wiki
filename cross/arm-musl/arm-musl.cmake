# vcpkg triplet: armv7 musl, fully static (see toolchain.cmake for why
# musl+static rather than a glibc cross-toolchain). Used via
# --overlay-triplets=cross/arm-musl --triplet arm-musl.

set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Release only — without this, vcpkg builds BOTH debug and release
# variants, and CMake's config-generator-expression matching between our
# project's RelWithDebInfo and each port's exported Debug/Release configs
# was picking a genuinely INCONSISTENT mix (release sqlite3/argon2/openssl
# alongside debug trantor/drogon/yaml-cpp/tomlplusplus/md4c) — plausibly
# the real cause of the linker SIGSEGV below, not just wasted build time.
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain.cmake)

# brotli's CLI tool crashes zig's bundled lld while linking for this
# target (SIGSEGV in the linker itself, not our code) and isn't needed
# anyway (only the libraries are, for Drogon's optional compression
# support) — see cross/overlay-ports/brotli, used via --overlay-ports.
