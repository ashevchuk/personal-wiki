#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wikitray {

enum class AgentError {
  None,
  Unauthorized,      // 401/403 — bad or revoked token
  ServiceUnavailable, // 503 — no LLM configured server-side
  RateLimited,        // 429
  NetworkError,       // request never got a response at all
  Other,               // any other non-200
};

struct AgentResult {
  AgentError error = AgentError::None;
  std::string reply;
  std::vector<std::string> documentsCreated;
  int httpStatus = 0;
  std::string errorBody;
};

// Wraps POST {server}/mcp/agent — see docs/mcp.md's "Remote agent endpoint"
// section in the wiki repo for the wire contract. Owns the growing
// conversation for its own lifetime; the server itself keeps no memory
// between calls, so every call resends the full history.
class AgentClient {
 public:
  AgentClient(std::string serverUrl, std::string token);

  AgentResult sendTurn(const std::string& userContent);

 private:
  std::string origin_;
  std::string agentPath_;
  std::string token_;
  nlohmann::json messages_ = nlohmann::json::array();
};

}  // namespace wikitray
