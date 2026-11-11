// CPPHTTPLIB_OPENSSL_SUPPORT is defined here, in this one translation unit,
// before including the vendored header — same reasoning as the wiki repo's
// own CloudEmbeddingProvider.cpp: cpp-mcp itself builds httplib.h without
// SSL support, but the header is header-only, so this file opts itself in
// independently.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "AgentClient.h"

#include <stdexcept>

namespace wikitray {

namespace {

constexpr const char* kAgentSuffix = "/mcp/agent";

// Splits "https://wiki.example.com/wiki" into origin
// ("https://wiki.example.com") and path prefix ("/wiki") — httplib::Client
// takes only the origin; the prefix is prepended to every request path.
// Same split as the wiki repo's own CloudEmbeddingProvider.cpp::parseApiBase.
struct ParsedServerUrl {
  std::string origin;
  std::string agentPath;
};

ParsedServerUrl parseServerUrl(std::string serverUrl) {
  while (!serverUrl.empty() && serverUrl.back() == '/') {
    serverUrl.pop_back();
  }

  const auto schemeEnd = serverUrl.find("://");
  if (schemeEnd == std::string::npos) {
    throw std::runtime_error(
        "AgentClient: server url must be an absolute http(s) URL (got \"" +
        serverUrl + "\")");
  }
  const std::string scheme = serverUrl.substr(0, schemeEnd);
  if (scheme != "http" && scheme != "https") {
    throw std::runtime_error(
        "AgentClient: server url must use http or https (got scheme \"" +
        scheme + "\")");
  }
  const std::string rest = serverUrl.substr(schemeEnd + 3);
  const auto slash = rest.find('/');
  const std::string hostport =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string pathPrefix =
      slash == std::string::npos ? std::string() : rest.substr(slash);
  if (hostport.empty()) {
    throw std::runtime_error("AgentClient: server url has no host (got \"" +
                              serverUrl + "\")");
  }

  ParsedServerUrl out;
  out.origin = scheme + "://" + hostport;
  out.agentPath = pathPrefix + kAgentSuffix;
  return out;
}

}  // namespace

AgentClient::AgentClient(std::string serverUrl, std::string token)
    : token_(std::move(token)) {
  const auto parsed = parseServerUrl(std::move(serverUrl));
  origin_ = parsed.origin;
  agentPath_ = parsed.agentPath;
}

AgentResult AgentClient::sendTurn(const std::string& userContent) {
  messages_.push_back({{"role", "user"}, {"content", userContent}});

  httplib::Client client(origin_);
  client.set_bearer_token_auth(token_);
  client.set_connection_timeout(10);
  client.set_read_timeout(60);

  const nlohmann::json requestBody{{"messages", messages_}};
  const auto res =
      client.Post(agentPath_, requestBody.dump(), "application/json");

  AgentResult result;
  if (!res) {
    result.error = AgentError::NetworkError;
    return result;
  }
  result.httpStatus = res->status;

  if (res->status == 401 || res->status == 403) {
    result.error = AgentError::Unauthorized;
    result.errorBody = res->body;
    return result;
  }
  if (res->status == 503) {
    result.error = AgentError::ServiceUnavailable;
    result.errorBody = res->body;
    return result;
  }
  if (res->status == 429) {
    result.error = AgentError::RateLimited;
    result.errorBody = res->body;
    return result;
  }
  if (res->status != 200) {
    result.error = AgentError::Other;
    result.errorBody = res->body;
    return result;
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(res->body);
  } catch (const nlohmann::json::parse_error&) {
    result.error = AgentError::Other;
    result.errorBody = res->body;
    return result;
  }

  result.reply = parsed.value("reply", std::string());
  if (parsed.contains("documentsCreated")) {
    for (const auto& doc : parsed["documentsCreated"]) {
      result.documentsCreated.push_back(doc.get<std::string>());
    }
  }

  messages_.push_back({{"role", "assistant"}, {"content", result.reply}});
  return result;
}

}  // namespace wikitray
