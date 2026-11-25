#include "core/McpProtocol.h"

#include "util/Base64.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace wikicore::mcp {

namespace {

constexpr std::array<std::string_view, 4> kLegacyProtocolVersions = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};

constexpr std::string_view kBase64SentinelPrefix = "=?base64?";
constexpr std::string_view kBase64SentinelSuffix = "?=";

ProtocolError headerMismatch(std::string message) {
  return ProtocolError{kErrorHeaderMismatch, "Header mismatch: " + std::move(message), {}};
}

// RFC 9110 field-value characters: visible ASCII, space, horizontal tab.
bool isPlainHeaderValue(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u == '\t' || (u >= 0x20 && u <= 0x7e);
  });
}

std::string toLowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// "host:port" -> "host"; "[::1]:8080" -> "[::1]"; no port -> unchanged.
std::string_view hostWithoutPort(std::string_view hostPort) {
  if (!hostPort.empty() && hostPort.front() == '[') {
    const std::size_t close = hostPort.find(']');
    return close == std::string_view::npos ? hostPort : hostPort.substr(0, close + 1);
  }
  return hostPort.substr(0, hostPort.find(':'));
}

}  // namespace

bool isLegacyProtocolVersion(std::string_view version) {
  return std::find(kLegacyProtocolVersions.begin(), kLegacyProtocolVersions.end(), version) !=
         kLegacyProtocolVersions.end();
}

bool isModernRequest(const RequestMeta& meta,
                     const std::optional<std::string>& protocolVersionHeader) {
  if (meta.hasProtocolVersionKey) return true;
  return protocolVersionHeader && !isLegacyProtocolVersion(*protocolVersionHeader);
}

std::optional<ProtocolError> checkRequiredMeta(const RequestMeta& meta) {
  if (!meta.protocolVersion) {
    return ProtocolError{kErrorInvalidParams,
                         "missing or invalid _meta." + std::string(kMetaProtocolVersion),
                         {}};
  }
  if (!meta.hasClientCapabilities) {
    return ProtocolError{kErrorInvalidParams,
                         "missing or invalid _meta." + std::string(kMetaClientCapabilities),
                         {}};
  }
  return std::nullopt;
}

std::optional<ProtocolError> checkProtocolVersion(std::string_view version) {
  if (version == kModernProtocolVersion) return std::nullopt;
  return ProtocolError{kErrorUnsupportedProtocolVersion, "Unsupported protocol version",
                       std::string(version)};
}

bool methodRequiresNameHeader(std::string_view method) {
  return method == "tools/call" || method == "resources/read" || method == "prompts/get";
}

std::optional<ProtocolError> checkModernHttpHeaders(const ModernHttpHeaders& headers,
                                                    std::string_view bodyVersion,
                                                    std::string_view bodyMethod,
                                                    const std::optional<std::string>& bodyName) {
  if (!headers.protocolVersion) return headerMismatch("missing MCP-Protocol-Version header");
  if (*headers.protocolVersion != bodyVersion) {
    return headerMismatch("MCP-Protocol-Version header value '" + *headers.protocolVersion +
                          "' does not match body value '" + std::string(bodyVersion) + "'");
  }
  if (!headers.method) return headerMismatch("missing Mcp-Method header");
  if (*headers.method != bodyMethod) {
    return headerMismatch("Mcp-Method header value '" + *headers.method +
                          "' does not match body value '" + std::string(bodyMethod) + "'");
  }
  if (!methodRequiresNameHeader(bodyMethod)) return std::nullopt;
  if (!headers.name) return headerMismatch("missing Mcp-Name header");
  const auto decodedName = decodeHeaderValue(*headers.name);
  if (!decodedName) return headerMismatch("malformed Mcp-Name header");
  if (bodyName && *decodedName != *bodyName) {
    return headerMismatch("Mcp-Name header value does not match body value");
  }
  return std::nullopt;
}

std::optional<std::string> decodeHeaderValue(std::string_view raw) {
  if (!isPlainHeaderValue(raw)) return std::nullopt;
  const bool isSentinel =
      raw.size() >= kBase64SentinelPrefix.size() + kBase64SentinelSuffix.size() &&
      raw.substr(0, kBase64SentinelPrefix.size()) == kBase64SentinelPrefix &&
      raw.substr(raw.size() - kBase64SentinelSuffix.size()) == kBase64SentinelSuffix;
  if (!isSentinel) return std::string(raw);
  return util::decodeBase64(raw.substr(
      kBase64SentinelPrefix.size(),
      raw.size() - kBase64SentinelPrefix.size() - kBase64SentinelSuffix.size()));
}

bool originMatchesHost(std::string_view origin, std::string_view host) {
  if (host.empty()) return false;
  const auto schemeEnd = origin.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  const std::string originHost = toLowerAscii(origin.substr(schemeEnd + 3));
  const std::string requestHost = toLowerAscii(host);
  if (originHost == requestHost) return true;
  // A reverse proxy may forward Host without the port (nginx `$host`); then
  // only the host names can be compared.
  return hostWithoutPort(requestHost) == requestHost &&
         hostWithoutPort(originHost) == requestHost;
}

}  // namespace wikicore::mcp
