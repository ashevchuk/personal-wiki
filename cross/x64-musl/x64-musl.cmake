# vcpkg triplet: generic x86_64 musl, fully static (see toolchain.cmake for
# why musl+static rather than the host's own glibc). Used via
# --overlay-triplets=cross/x64-musl --triplet x64-musl.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Release only — see cross/arm-musl/arm-musl.cmake's comment on why a mixed
# debug/release port set caused a linker SIGSEGV there; kept consistent
# across every triplet rather than re-deriving it per target.
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain.cmake)

# cross/overlay-ports/{brotli,libuuid,md4c} patch around zig/lld crashes
# confirmed on the ARM targets — whether any of that applies on x86_64 is
# unverified until this triplet's first real build.
