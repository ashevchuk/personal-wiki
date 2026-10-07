# vcpkg triplet: ARM1176JZF-S (Raspberry Pi 1/Zero/Zero W), musl, fully
# static (see toolchain.cmake for why this core needs its own triplet,
# separate from ../arm-musl's ARMv7 one). Used via
# --overlay-triplets=cross/armv6-musl --triplet armv6-musl. vcpkg itself
# has no separate "armv6" architecture enum — VCPKG_TARGET_ARCHITECTURE
# is still "arm"; the actual ARM1176-vs-generic-v7a distinction lives
# entirely in this triplet's own toolchain.cmake (the compiler wrappers'
# -mcpu flag), invisible to vcpkg's own triplet machinery.

set(VCPKG_TARGET_ARCHITECTURE arm)
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
# different board" section. Whether these specific patches are still
# necessary on THIS core is unverified; try building without
# --overlay-ports first if that ever needs re-checking.
