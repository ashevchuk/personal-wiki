#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wikicore::llm {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;  // JSON object as a string, OpenAI's shape
};

struct ChatMessage {
  std::string role;  // system | user | assistant | tool
  std::string content;
  std::string toolCallId;  // role=tool
  std::vector<ToolCall> toolCalls;  // role=assistant
};

struct ChatCompletion {
  std::string content;
  std::vector<ToolCall> toolCalls;
  std::string finishReason;
};

// Outbound chat with tool-calling. Implemented by CloudChatClient
// (OpenAI-compatible POST {api_base}/chat/completions). The wiki is the
// HTTP *client* here — the cloud model never sees MCP, stdio or
// POST /mcp. Tools in the request are ordinary function schemas; this
// process executes them against the vault and sends the results back.
class ChatClient {
 public:
  virtual ~ChatClient() = default;
  virtual ChatCompletion complete(const std::vector<ChatMessage>& messages,
                                  const nlohmann::json& tools) = 0;
  // Abort an in-flight complete() from another thread. Default is a
  // no-op (scripted unit-test clients); CloudChatClient stops httplib.
  virtual void cancel() {}
};

}  // namespace wikicore::llm
