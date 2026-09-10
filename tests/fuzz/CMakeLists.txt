# Coverage-guided fuzz targets for the two highest-value untrusted-input
# parsers in the codebase (PathGuard::resolve, parseFrontMatter) — see
# each harness's own file comment for why these two specifically.
#
# Off by default (WIKI_ENABLE_FUZZING, default OFF) and gated on Clang:
# libFuzzer (-fsanitize=fuzzer) is a Clang/LLVM feature, not something
# GCC (this project's default compiler, per docs/architecture.md) ships.
# Configure with `-DWIKI_ENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++`
# to build these; they are NOT part of `ctest` (a fuzzer target has no
# fixed pass/fail — it runs until stopped or until it finds a crash) —
# run them directly instead, e.g.:
#
#   cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
#     -DWIKI_ENABLE_FUZZING=ON \
#     -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-fuzz --target fuzz_path_guard fuzz_front_matter
#   ./build-fuzz/tests/fuzz/fuzz_path_guard tests/fuzz/corpus/path_guard
#   ./build-fuzz/tests/fuzz/fuzz_front_matter tests/fuzz/corpus/front_matter
#
# Each links its ONE target source file directly (not libwikicore, which
# is built without sanitizer instrumentation) so ASan/UBSan actually
# cover the code under test — see the harness file comments.

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(STATUS "WIKI_ENABLE_FUZZING requires Clang (libFuzzer) — "
                  "current compiler is ${CMAKE_CXX_COMPILER_ID}, skipping fuzz targets")
  return()
endif()

set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -g -O1)

add_executable(fuzz_path_guard
  fuzz_path_guard.cpp
  ${CMAKE_SOURCE_DIR}/src/vault/PathGuard.cpp
)
target_include_directories(fuzz_path_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(fuzz_path_guard PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_path_guard PRIVATE ${FUZZ_FLAGS})

add_executable(fuzz_front_matter
  fuzz_front_matter.cpp
  ${CMAKE_SOURCE_DIR}/src/vault/FrontMatter.cpp
)
target_include_directories(fuzz_front_matter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(fuzz_front_matter PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_front_matter PRIVATE ${FUZZ_FLAGS})
target_link_libraries(fuzz_front_matter PRIVATE yaml-cpp::yaml-cpp)
