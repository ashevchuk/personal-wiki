#include "core/McpProtocol.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wikicore::mcp;

namespace {

RequestMeta modernMeta(std::string version = std::string(kModernProtocolVersion)) {
  RequestMeta meta;
  meta.hasProtocolVersionKey = true;
  meta.protocolVersion = std::move(version);
  meta.hasClientCapabilities = true;
  return meta;
}

ModernHttpHeaders headersFor(std::string method, std::optional<std::string> name = std::nullopt) {
  ModernHttpHeaders h;
  h.protocolVersion = std::string(kModernProtocolVersion);
  h.method = std::move(method);
  h.name = std::move(name);
  return h;
}

}  // namespace

TEST_CASE("isLegacyProtocolVersion knows the initialize-era revisions", "[McpProtocol]") {
  REQUIRE(isLegacyProtocolVersion("2024-11-05"));
  REQUIRE(isLegacyProtocolVersion("2025-03-26"));
  REQUIRE(isLegacyProtocolVersion("2025-06-18"));
  REQUIRE(isLegacyProtocolVersion("2025-11-25"));
  REQUIRE_FALSE(isLegacyProtocolVersion("2026-07-28"));
  REQUIRE_FALSE(isLegacyProtocolVersion(""));
}

TEST_CASE("isModernRequest: body metadata or a non-legacy header selects the modern era",
          "[McpProtocol]") {
  REQUIRE(isModernRequest(modernMeta(), std::nullopt));
  REQUIRE_FALSE(isModernRequest(RequestMeta{}, std::nullopt));
  // Legacy clients since 2025-06-18 send MCP-Protocol-Version with no _meta.
  REQUIRE_FALSE(isModernRequest(RequestMeta{}, std::string("2025-06-18")));
  // Claims the modern revision but has no body metadata: must be modern so
  // that it fails validation instead of being served as legacy.
  REQUIRE(isModernRequest(RequestMeta{}, std::string("2026-07-28")));
  REQUIRE(isModernRequest(RequestMeta{}, std::string("2099-01-01")));
}

TEST_CASE("checkRequiredMeta rejects missing or mistyped fields with -32602", "[McpProtocol]") {
  REQUIRE_FALSE(checkRequiredMeta(modernMeta()).has_value());

  RequestMeta noVersion = modernMeta();
  noVersion.hasProtocolVersionKey = false;
  noVersion.protocolVersion.reset();
  REQUIRE(checkRequiredMeta(noVersion)->code == kErrorInvalidParams);

  RequestMeta versionNotString = modernMeta();
  versionNotString.protocolVersion.reset();
  REQUIRE(checkRequiredMeta(versionNotString)->code == kErrorInvalidParams);

  RequestMeta noCapabilities = modernMeta();
  noCapabilities.hasClientCapabilities = false;
  REQUIRE(checkRequiredMeta(noCapabilities)->code == kErrorInvalidParams);
}

TEST_CASE("checkProtocolVersion accepts only the modern revision", "[McpProtocol]") {
  REQUIRE_FALSE(checkProtocolVersion("2026-07-28").has_value());
  const auto err = checkProtocolVersion("1900-01-01");
  REQUIRE(err.has_value());
  REQUIRE(err->code == kErrorUnsupportedProtocolVersion);
  REQUIRE(err->requestedVersion == "1900-01-01");
  // A legacy revision inside modern metadata is not served on the modern path.
  REQUIRE(checkProtocolVersion("2025-03-26")->code == kErrorUnsupportedProtocolVersion);
}

TEST_CASE("methodRequiresNameHeader", "[McpProtocol]") {
  REQUIRE(methodRequiresNameHeader("tools/call"));
  REQUIRE(methodRequiresNameHeader("resources/read"));
  REQUIRE(methodRequiresNameHeader("prompts/get"));
  REQUIRE_FALSE(methodRequiresNameHeader("tools/list"));
  REQUIRE_FALSE(methodRequiresNameHeader("server/discover"));
}

TEST_CASE("checkModernHttpHeaders: matching headers pass", "[McpProtocol]") {
  REQUIRE_FALSE(checkModernHttpHeaders(headersFor("tools/list"), "2026-07-28", "tools/list",
                                       std::nullopt)
                    .has_value());
  REQUIRE_FALSE(checkModernHttpHeaders(headersFor("tools/call", "get_document"), "2026-07-28",
                                       "tools/call", std::string("get_document"))
                    .has_value());
}

TEST_CASE("checkModernHttpHeaders: missing or mismatched headers are -32020", "[McpProtocol]") {
  ModernHttpHeaders noVersion = headersFor("tools/list");
  noVersion.protocolVersion.reset();
  REQUIRE(checkModernHttpHeaders(noVersion, "2026-07-28", "tools/list", std::nullopt)->code ==
          kErrorHeaderMismatch);

  REQUIRE(checkModernHttpHeaders(headersFor("tools/list"), "2099-01-01", "tools/list",
                                 std::nullopt)
              ->code == kErrorHeaderMismatch);

  ModernHttpHeaders noMethod = headersFor("tools/list");
  noMethod.method.reset();
  REQUIRE(checkModernHttpHeaders(noMethod, "2026-07-28", "tools/list", std::nullopt)->code ==
          kErrorHeaderMismatch);

  REQUIRE(checkModernHttpHeaders(headersFor("tools/list"), "2026-07-28", "tools/call",
                                 std::string("x"))
              ->code == kErrorHeaderMismatch);

  REQUIRE(checkModernHttpHeaders(headersFor("tools/call"), "2026-07-28", "tools/call",
                                 std::string("get_document"))
              ->code == kErrorHeaderMismatch);

  REQUIRE(checkModernHttpHeaders(headersFor("tools/call", "foo"), "2026-07-28", "tools/call",
                                 std::string("bar"))
              ->code == kErrorHeaderMismatch);
}

TEST_CASE("checkModernHttpHeaders: missing body name is left to the handler", "[McpProtocol]") {
  REQUIRE_FALSE(checkModernHttpHeaders(headersFor("tools/call", "foo"), "2026-07-28",
                                       "tools/call", std::nullopt)
                    .has_value());
}

TEST_CASE("checkModernHttpHeaders decodes a base64 sentinel Mcp-Name", "[McpProtocol]") {
  // "Hello, 世界" from the spec's encoding table.
  REQUIRE_FALSE(checkModernHttpHeaders(headersFor("tools/call", "=?base64?SGVsbG8sIOS4lueVjA==?="),
                                       "2026-07-28", "tools/call",
                                       std::string("Hello, \xe4\xb8\x96\xe7\x95\x8c"))
                    .has_value());
}

TEST_CASE("decodeHeaderValue", "[McpProtocol]") {
  REQUIRE(decodeHeaderValue("us-west1") == std::string("us-west1"));
  REQUIRE(decodeHeaderValue("=?base64?IHBhZGRlZCA=?=") == std::string(" padded "));
  REQUIRE(decodeHeaderValue("=?base64?bGluZTEKbGluZTI=?=") == std::string("line1\nline2"));
  REQUIRE(decodeHeaderValue("=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?=") ==
          std::string("=?base64?literal?="));
  REQUIRE_FALSE(decodeHeaderValue("=?base64?***?=").has_value());
  REQUIRE_FALSE(decodeHeaderValue("bad\x01value").has_value());
  REQUIRE_FALSE(decodeHeaderValue("caf\xc3\xa9").has_value());
  // Markers are case-sensitive: an uppercase sentinel is a plain value.
  REQUIRE(decodeHeaderValue("=?BASE64?eA==?=") == std::string("=?BASE64?eA==?="));
}

TEST_CASE("originMatchesHost", "[McpProtocol]") {
  REQUIRE(originMatchesHost("https://wiki.example", "wiki.example"));
  REQUIRE(originMatchesHost("http://127.0.0.1:8080", "127.0.0.1:8080"));
  REQUIRE(originMatchesHost("https://Wiki.Example", "wiki.example"));
  REQUIRE_FALSE(originMatchesHost("https://evil.example", "wiki.example"));
  REQUIRE_FALSE(originMatchesHost("https://wiki.example.evil", "wiki.example"));
  REQUIRE_FALSE(originMatchesHost("http://127.0.0.1:9999", "127.0.0.1:8080"));
  // Proxy forwarded Host without the port (nginx $host).
  REQUIRE(originMatchesHost("https://wiki.example:8443", "wiki.example"));
  REQUIRE(originMatchesHost("http://[::1]:8080", "[::1]"));
  REQUIRE_FALSE(originMatchesHost("https://evil.example:8443", "wiki.example"));
  REQUIRE_FALSE(originMatchesHost("https://wiki.example.evil:8443", "wiki.example"));
  // Host with a port must match the Origin's port exactly.
  REQUIRE_FALSE(originMatchesHost("https://wiki.example", "wiki.example:8443"));
  REQUIRE_FALSE(originMatchesHost("null", "wiki.example"));
  REQUIRE_FALSE(originMatchesHost("https://wiki.example", ""));
}
