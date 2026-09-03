vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/brotli
    REF v${VERSION} # v1.1.0 
    SHA512 f94542afd2ecd96cc41fd21a805a3da314281ae558c10650f3e6d9ca732b8425bba8fde312823f0a564c7de3993bdaab5b43378edab65ebb798cefb6fd702256
    HEAD_REF master
    PATCHES
        install.patch
        pkgconfig.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBROTLI_DISABLE_TESTS=ON
        # Local overlay change: the CLI tool isn't needed (only the
        # libraries are, for Drogon's optional compression support), and
        # linking it crashes zig's bundled lld for the arm-musl cross
        # target (SIGSEGV in the linker itself, not our code — see
        # docs/architecture.md). Skip building it entirely.
        -DBROTLI_BUILD_TOOLS=OFF
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH share/unofficial-brotli PACKAGE_NAME unofficial-brotli)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/tools")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/man")

# Local overlay change: no CLI tool was built (BROTLI_BUILD_TOOLS=OFF
# above), so there's nothing for vcpkg_copy_tools to find — the upstream
# call here is deliberately removed rather than made conditional, since
# this overlay port's whole point is "never build the tool."

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
