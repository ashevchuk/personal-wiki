// libFuzzer harness for wikicore::vault::parseFrontMatter() --
// FrontMatter.h's contract is explicit: "This function never throws",
// because it has to survive hand-edited YAML from outside the app (see
// docs/architecture.md, front-matter parsing rationale for using
// yaml-cpp instead of a hand-rolled parser -- real YAML edge cases
// matter here). A fuzzer input that makes it throw is a direct contract
// violation, not a hypothetical -- and yaml-cpp is a large enough parser
// that "never throws" is a claim worth continuously verifying, not just
// asserting once in FrontMatterTest.cpp's fixed cases.
//
// Deliberately links FrontMatter.cpp directly (see tests/fuzz/CMakeLists.txt)
// rather than against the prebuilt wikicore static library, so ASan/UBSan
// instrumentation covers this project's own code; yaml-cpp itself stays
// an uninstrumented prebuilt vcpkg dependency, same tradeoff every fuzz
// target in this codebase makes for its third-party deps.
#include "vault/FrontMatter.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  // No try/catch here on purpose: the documented contract is "never
  // throws" for ANY input, malformed or not -- an escaping exception IS
  // the bug this harness exists to find, and libFuzzer correctly reports
  // it as a crash.
  (void)wikicore::vault::parseFrontMatter(input);
  return 0;
}
