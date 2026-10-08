# vcpkg triplet: generic AArch64 musl, fully static (see toolchain.cmake
# for why this target exists separately from ../arm-musl/'s 32-bit
# ARMv7 one — the test board's kernel has CONFIG_COMPAT unset, so a
# 32-bit binary can't execute at all, not just run slower). Used via
# --overlay-triplets=cross/aarch64-musl --triplet aarch64-musl. vcpkg's
# own architecture enum spells this "arm64", not "aarch64" — the actual
# CMAKE_SYSTEM_PROCESSOR/compiler-target spelling lives in
# toolchain.cmake instead.

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Release only — same reasoning as ../arm-musl/arm-musl.cmake: avoids a
# debug/release config mismatch across ports that plausibly caused a
# linker SIGSEGV there. Unverified on this architecture specifically.
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain.cmake)

# Same cross/overlay-ports/ (brotli, libuuid, md4c) as ../arm-musl/ —
# not triplet-specific, see docs/sbc-deployment.md's "Porting to a
# different board" section. Whether these specific patches are needed
# on this architecture is unverified; try building without
# --overlay-ports first if that ever needs re-checking.
