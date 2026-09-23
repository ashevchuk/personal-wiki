#pragma once

#include "llm/ChatClient.h"

#include <map>
#include <string>
#include <string_view>

namespace wikicore::llm {

// Assembles an OpenAI-compatible chat.completion.chunk SSE stream
// (`data: {...}\n\n` plus a terminal `data: [DONE]`) into one ChatCompletion.
// Tool-call arguments arrive as deltas keyed by `index` and are concatenated.
// A provider that ignores `stream:true` and returns one JSON object is
// accepted in finish() so the same path works for both shapes.
class ChatStreamParser {
 public:
  void feed(std::string_view bytes, const ChatDeltaFn& onDelta);
  ChatCompletion finish(const ChatDeltaFn& onDelta);

  const ChatCompletion& completion() const { return out_; }
  bool receivedDone() const { return done_; }
  bool sawSse() const { return sawSse_; }

  static ChatCompletion parseJsonCompletion(const nlohmann::json& parsed);

 private:
  void consumeEvent(std::string_view event, const ChatDeltaFn& onDelta);
  void applyChunk(const nlohmann::json& chunk, const ChatDeltaFn& onDelta);
  void applyDelta(const nlohmann::json& delta, const ChatDeltaFn& onDelta);
  void flushTools();

  std::string buffer_;
  ChatCompletion out_;
  std::map<int, ToolCall> tools_;
  bool done_ = false;
  bool sawSse_ = false;
};

}  // namespace wikicore::llm
