#pragma once

#include "llm/ChatClient.h"

#include <atomic>
#include <mutex>
#include <string>

namespace wikicore::llm {

// OpenAI-compatible POST {api_base}/chat/completions. Claude works when
// api_base is https://api.anthropic.com/v1 and the key is Anthropic's —
// that host's compatibility layer speaks this same JSON. The wiki never
// attaches MCP; tools are function schemas in this request.
class CloudChatClient : public ChatClient {
 public:
  CloudChatClient(const std::string& apiKeyEnvVar, std::string apiBase,
                  std::string model);

  ChatCompletion complete(const std::vector<ChatMessage>& messages,
                          const nlohmann::json& tools) override;
  void cancel() override;

 private:
  std::string origin_;
  std::string chatPath_;
  std::string model_;
  std::string apiKey_;
  std::mutex requestMu_;
  std::atomic<bool> stopRequested_{false};
  void* activeClient_ = nullptr;  // httplib::Client*, only while Post is in flight
};

}  // namespace wikicore::llm
