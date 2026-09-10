// libFuzzer harness for wikicore::vault::PathGuard::resolve() --
// src/vault/PathGuard.h's own comment names this "the single point of
// contact between an untrusted path string and the real filesystem",
// which makes it the highest-value fuzz target in the codebase: any
// input that escapes with something other than a documented, controlled
// exception is a traversal bug PathGuardTest.cpp's fixed payload list
// (../../etc/passwd, %2e%2e, symlink escapes) was never going to find,
// by construction -- those are known patterns, this hunts for unknown
// ones via coverage-guided mutation instead.
//
// Deliberately links PathGuard.cpp directly (see tests/fuzz/CMakeLists.txt)
// rather than against the prebuilt wikicore static library, so ASan/UBSan
// instrumentation actually covers the code under test instead of calling
// into an uninstrumented .a.
#include "vault/PathGuard.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace {

// Built once and reused across every iteration -- PathGuard's constructor
// canonicalizes a real directory, not worth paying for per-input.
wikicore::vault::PathGuard& guard() {
  static const std::filesystem::path root = [] {
    auto p = std::filesystem::temp_directory_path() / "wiki-fuzz-pathguard-vault";
    std::filesystem::create_directories(p);
    return p;
  }();
  static wikicore::vault::PathGuard g(root);
  return g;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  try {
    (void)guard().resolve(input);
  } catch (const wikicore::vault::PathTraversalError&) {
    // Expected, controlled rejection -- resolve()'s documented contract
    // for exactly this kind of input.
  } catch (const std::filesystem::filesystem_error&) {
    // Also documented as possible (PathGuard.h's constructor comment) --
    // e.g. a component that's syntactically fine but the filesystem
    // itself rejects (see the real NAME_MAX finding this stress-testing
    // round already turned up one layer up, in DocumentService).
  }
  // Anything else escaping -- any other exception type, an assertion, a
  // sanitizer report -- is exactly what this harness exists to catch,
  // and is deliberately NOT caught here.
  return 0;
}
