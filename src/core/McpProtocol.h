#pragma once

#include <optional>
#include <string>
#include <string_view>

// Transport-independent rules of the MCP 2026-07-28 ("modern", stateless)
// protocol revision, shared by the stdio server (src/mcp/McpServer.cpp,
// nlohmann::json) and the remote HTTP transport
// (src/controllers/RemoteMcpRoutes.cpp, jsoncpp). Each transport extracts
// plain values from its own JSON type and builds its own responses; the
// decisions (which era, which error) are made only here.
//
// Both transports are dual-era: an `initialize` request, or any request
// without modern per-request metadata, keeps the 2025-03-26 behavior.
namespace wikicore::mcp {

inline constexpr std::string_view kModernProtocolVersion = "2026-07-28";
inline constexpr std::string_view kLegacyProtocolVersion = "2025-03-26";

inline constexpr std::string_view kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view kMetaClientCapabilities =
    "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

inline constexpr int kErrorMethodNotFound = -32601;
inline constexpr int kErrorInvalidParams = -32602;
inline constexpr int kErrorHeaderMismatch = -32020;
inline constexpr int kErrorUnsupportedProtocolVersion = -32022;

inline constexpr long long kDiscoverTtlMs = 3'600'000;
inline constexpr long long kToolsListTtlMs = 300'000;

// The parts of a request's `params._meta` that decide its era and
// validity, extracted by the transport from its own JSON representation.
struct RequestMeta {
  bool hasProtocolVersionKey = false;
  // Set only when the key is present AND its value is a string.
  std::optional<std::string> protocolVersion;
  // Present AND a JSON object.
  bool hasClientCapabilities = false;
};

struct ProtocolError {
  int code = 0;
  std::string message;
  // Set only for kErrorUnsupportedProtocolVersion (its `data.requested`).
  std::string requestedVersion;
};

// The HTTP headers that 2026-07-28 Streamable HTTP mirrors from the body.
// nullopt = header absent or empty.
struct ModernHttpHeaders {
  std::optional<std::string> protocolVersion;  // MCP-Protocol-Version
  std::optional<std::string> method;           // Mcp-Method
  std::optional<std::string> name;             // Mcp-Name
};

// Protocol revisions that use the `initialize` handshake.
bool isLegacyProtocolVersion(std::string_view version);

// A request is served under 2026-07-28 rules when its body carries the
// per-request protocol version, or (HTTP only) its MCP-Protocol-Version
// header names a non-legacy revision. The header case matters for a request
// that claims the modern revision but omits the body metadata: it must fail
// as a malformed modern request, not be silently served as legacy.
// `initialize` is never modern; callers check that first.
bool isModernRequest(const RequestMeta& meta,
                     const std::optional<std::string>& protocolVersionHeader);

// -32602 when a required per-request `_meta` field is missing or mistyped.
std::optional<ProtocolError> checkRequiredMeta(const RequestMeta& meta);

// -32022 when the (already present) protocol version is not served.
std::optional<ProtocolError> checkProtocolVersion(std::string_view version);

// Methods whose Mcp-Name header mirrors `params.name` / `params.uri`.
bool methodRequiresNameHeader(std::string_view method);

// -32020 when a required mirrored header is missing, malformed, or differs
// from the body. `bodyName` is `params.name` / `params.uri` when the body
// has it as a string; a missing body value is left for the method handler
// to reject.
std::optional<ProtocolError> checkModernHttpHeaders(const ModernHttpHeaders& headers,
                                                    std::string_view bodyVersion,
                                                    std::string_view bodyMethod,
                                                    const std::optional<std::string>& bodyName);

// Page size and offset of the search/list tools. A negative limit would
// reach SQLite as "no limit", so anything outside [1, kMaxToolLimit] falls
// back or is capped; a negative offset becomes 0.
inline constexpr int kMaxToolLimit = 200;
int clampToolLimit(long long requested, int fallback);
int clampToolOffset(long long requested);

// Tool error text for arguments of the wrong JSON type, same on both
// transports.
inline constexpr std::string_view kInvalidToolArguments =
    "invalid arguments: 'arguments' must be an object and each parameter must have the type its "
    "input schema gives";

// Decodes a header value that may use the `=?base64?...?=` sentinel.
// nullopt when the value contains characters not allowed in a plain header
// value, or the sentinel payload is not valid Base64.
std::optional<std::string> decodeHeaderValue(std::string_view raw);

// True when an Origin header value names the same host[:port] as the Host
// header, or the same host when Host carries no port (a proxy forwarding
// nginx's `$host`). Used to reject cross-origin browser requests (DNS
// rebinding).
bool originMatchesHost(std::string_view origin, std::string_view host);

}  // namespace wikicore::mcp
