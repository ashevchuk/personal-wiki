vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mity/md4c
    REF "release-${VERSION}"
    SHA512 213d6b9fbad24b2bfb4fa0a8124cb4c20861da2cb57790882aa0e5ff8c18903450f1d9ffdbcc0547debd103137777059f27a526cd818294f698b5ffdbfe7fbcb
    HEAD_REF master
    PATCHES
        "cmake.patch"
        # Backport of upstream commit ecbb091b5c94211b51f04e0418e38ecb6d786e6f
        # ("md_analyze_table_alignment: Bound the dash scan by the row end"),
        # merged to md4c's main branch 2026-06-17 but not yet in any tagged
        # release as of release-0.5.3 (the version this port still pins).
        # Fixes OSV-2022-126 (google/oss-fuzz-vulns): a heap-buffer-overflow
        # READ in md_analyze_table_alignment(), reachable by rendering a
        # malformed GFM table -- confirmed present in release-0.5.3 and
        # confirmed absent on current upstream HEAD via OSV.dev's commit-based
        # query API (POST https://api.osv.dev/v1/query {"commit": "<sha>"}).
        # See docs/architecture.md for the full investigation writeup.
        # Applies cleanly against release-0.5.3 unmodified (verified
        # 2026-09-10) -- drop this patch (and this overlay port entirely,
        # reverting to the upstream vcpkg port) once vcpkg.json's
        # builtin-baseline picks up a md4c release that already contains it.
        "0001-md_analyze_table_alignment-fix-oob-read-OSV-2022-126.patch"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS -DBUILD_MD2HTML_EXECUTABLE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/md4c")
vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
