# Cross-compile toolchain: ARM1176JZF-S (the exact core in the BCM2835
# SoC — Raspberry Pi 1/Zero/Zero W), ARMv6, musl libc, fully static — via
# zig cc/c++ (bundled libc/libc++, no separate toolchain download). Same
# shape as ../arm-musl/ (that one targets ARMv7 generic — a Pi 2/3/4 in
# 32-bit mode, or most 32-bit-capable SBCs); this one exists because
# ARMv7 is NOT backward-compatible with ARMv6 — a v7-targeted binary
# executes an illegal instruction on a true ARMv6 core, it isn't just
# "slower," it crashes outright. `-mcpu=arm1176jzf_s` pins the EXACT
# core rather than a generic ARMv6 baseline, since "ARMv6" itself covers
# several incompatible variants (ARMv6-M, ARMv6K, ARMv6Z...) and the
# real target here is one specific, known chip. Underscore before the
# final `s`, not a hyphen — zig's `-mcpu` syntax treats a trailing
# `-name` as "disable feature `name`" (the same `+`/`-` syntax
# ../arm-musl/'s own `-mcpu=generic+v7a` uses to ADD a feature), so
# `arm1176jzf-s` parsed as "cpu=arm1176jzf, disable feature s" and
# failed with "unknown CPU: 'arm1176jzf'" — confirmed live, this is
# what broke every CI build of this triplet.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ARM1176JZF-S has NO NEON unit — root CMakeLists.txt's
# WIKI_ENABLE_LOCAL_EMBEDDINGS block reads this to skip the
# -mcpu=generic+v7a+fp16 override ../arm-musl/'s own toolchain sets
# WIKI_CROSS_ARM_HAS_NEON=TRUE for. Applying that override here forces
# ARMv7+NEON codegen onto a true ARMv6 core, which illegal-instruction-
# crashes outright rather than just running slower — confirmed live
# (qemu-arm-static --cpu arm1176, SIGILL in ggml's own ARM NEON code).
# Without the override, ggml's own compile-time feature probing (see
# ggml-cpu/CMakeLists.txt's ARM branch) correctly detects this compiler
# target has no NEON/dotprod/fp16-vector support and falls back to a
# plain scalar implementation instead.
set(WIKI_CROSS_ARM_HAS_NEON FALSE)

set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/cc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/c++)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/ranlib CACHE FILEPATH "")

set(CMAKE_C_COMPILER_TARGET arm-linux-musleabihf)
set(CMAKE_CXX_COMPILER_TARGET arm-linux-musleabihf)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Carried over from ../arm-musl/toolchain.cmake, UNVERIFIED on this
# architecture (see that file's own comment and docs/sbc-deployment.md's
# "Porting to a different board" section): the zig/lld SIGSEGV workaround
# and module-scanning workaround were found on ARMv7, plausibly generic
# to zig's cross-linking path regardless of -mcpu, but not confirmed on
# a second architecture until this one actually gets built and tested.
# If the build links cleanly without these two lines, drop them; if it
# SIGSEGVs the same way (code=139), leave them — same fix either way.
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
