#include "llm/ChatStreamParser.h"

#include <stdexcept>

namespace wikicore::llm {

namespace {

std::string_view trimLeft(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  return s;
}

std::string errorMessage(const nlohmann::json& parsed) {
  if (!parsed.contains("error")) return {};
  const auto& err = parsed["error"];
  if (err.is_string()) return err.get<std::string>();
  if (err.is_object() && err.contains("message") && err["message"].is_string()) {
    return err["message"].get<std::string>();
  }
  return err.dump();
}

}  // namespace

ChatCompletion ChatStreamParser::parseJsonCompletion(const nlohmann::json& parsed) {
  if (const auto msg = errorMessage(parsed); !msg.empty()) {
    throw std::runtime_error("chat API error: " + msg);
  }
  if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
      parsed["choices"].empty()) {
    throw std::runtime_error("chat API response has no choices");
  }
  const auto& choice = parsed["choices"][0];
  const auto& message = choice.contains("message") ? choice["message"] : choice;
  ChatCompletion out;
  if (message.contains("content") && message["content"].is_string()) {
    out.content = message["content"].get<std::string>();
  }
  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    out.finishReason = choice["finish_reason"].get<std::string>();
  }
  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto& call : message["tool_calls"]) {
      ToolCall tc;
      tc.id = call.value("id", "");
      if (call.contains("function") && call["function"].is_object()) {
        tc.name = call["function"].value("name", "");
        if (call["function"].contains("arguments")) {
          if (call["function"]["arguments"].is_string()) {
            tc.arguments = call["function"]["arguments"].get<std::string>();
          } else {
            tc.arguments = call["function"]["arguments"].dump();
          }
        }
      }
      out.toolCalls.push_back(std::move(tc));
    }
  }
  return out;
}

void ChatStreamParser::flushTools() {
  if (tools_.empty()) return;
  out_.toolCalls.clear();
  for (auto& [index, call] : tools_) {
    (void)index;
    out_.toolCalls.push_back(call);
  }
}

void ChatStreamParser::applyDelta(const nlohmann::json& delta, const ChatDeltaFn& onDelta) {
  if (delta.contains("content") && delta["content"].is_string()) {
    const auto piece = delta["content"].get<std::string>();
    if (!piece.empty()) {
      out_.content += piece;
      if (onDelta) onDelta(piece);
    }
  }
  if (!delta.contains("tool_calls") || !delta["tool_calls"].is_array()) return;
  for (const auto& call : delta["tool_calls"]) {
    const int index = call.value("index", 0);
    ToolCall& tc = tools_[index];
    if (call.contains("id") && call["id"].is_string()) {
      const auto id = call["id"].get<std::string>();
      if (!id.empty()) tc.id = id;
    }
    if (!call.contains("function") || !call["function"].is_object()) continue;
    const auto& fn = call["function"];
    if (fn.contains("name") && fn["name"].is_string()) {
      const auto name = fn["name"].get<std::string>();
      if (!name.empty()) tc.name = name;
    }
    if (fn.contains("arguments")) {
      if (fn["arguments"].is_string()) {
        tc.arguments += fn["arguments"].get<std::string>();
      } else if (!fn["arguments"].is_null()) {
        tc.arguments = fn["arguments"].dump();
      }
    }
  }
}

void ChatStreamParser::applyChunk(const nlohmann::json& chunk, const ChatDeltaFn& onDelta) {
  if (const auto msg = errorMessage(chunk); !msg.empty()) {
    throw std::runtime_error("chat API error: " + msg);
  }
  if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
      chunk["choices"].empty()) {
    return;
  }
  const auto& choice = chunk["choices"][0];
  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    const auto reason = choice["finish_reason"].get<std::string>();
    if (!reason.empty() && reason != "null") out_.finishReason = reason;
  }
  if (choice.contains("delta") && choice["delta"].is_object()) {
    applyDelta(choice["delta"], onDelta);
    return;
  }
  if (choice.contains("message") && choice["message"].is_object()) {
    const auto parsed = parseJsonCompletion(chunk);
    if (out_.content.empty() && !parsed.content.empty()) {
      out_.content = parsed.content;
      if (onDelta) onDelta(parsed.content);
    }
    if (out_.toolCalls.empty() && !parsed.toolCalls.empty()) {
      out_.toolCalls = parsed.toolCalls;
      int i = 0;
      for (const auto& tc : parsed.toolCalls) tools_[i++] = tc;
    }
    if (out_.finishReason.empty()) out_.finishReason = parsed.finishReason;
  }
}

void ChatStreamParser::consumeEvent(std::string_view event, const ChatDeltaFn& onDelta) {
  std::string data;
  std::size_t lineStart = 0;
  while (lineStart <= event.size()) {
    const auto nl = event.find('\n', lineStart);
    std::string_view line = event.substr(
        lineStart, (nl == std::string_view::npos ? event.size() : nl) - lineStart);
    lineStart = nl == std::string_view::npos ? event.size() + 1 : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == ':') continue;
    constexpr std::string_view kData = "data:";
    if (line.size() < kData.size() || line.substr(0, kData.size()) != kData) continue;
    auto payload = trimLeft(line.substr(kData.size()));
    if (!data.empty()) data.push_back('\n');
    data.append(payload);
  }
  if (data.empty()) return;
  if (data == "[DONE]") {
    done_ = true;
    return;
  }
  nlohmann::json chunk;
  try {
    chunk = nlohmann::json::parse(data);
  } catch (const nlohmann::json::parse_error&) {
    return;
  }
  sawSse_ = true;
  applyChunk(chunk, onDelta);
}

void ChatStreamParser::feed(std::string_view bytes, const ChatDeltaFn& onDelta) {
  buffer_.append(bytes.data(), bytes.size());
  while (true) {
    auto pos = buffer_.find("\n\n");
    std::size_t sep = 2;
    const auto cr = buffer_.find("\r\n\r\n");
    if (cr != std::string::npos && (pos == std::string::npos || cr < pos)) {
      pos = cr;
      sep = 4;
    }
    if (pos == std::string::npos) break;
    const std::string event = buffer_.substr(0, pos);
    buffer_.erase(0, pos + sep);
    consumeEvent(event, onDelta);
  }
}

ChatCompletion ChatStreamParser::finish(const ChatDeltaFn& onDelta) {
  if (!buffer_.empty()) {
    const bool leftoverLooksSse =
        sawSse_ || buffer_.find("data:") != std::string::npos;
    if (leftoverLooksSse) {
      consumeEvent(buffer_, onDelta);
    } else {
      std::size_t start = 0;
      while (start < buffer_.size() &&
             (buffer_[start] == ' ' || buffer_[start] == '\n' ||
              buffer_[start] == '\r' || buffer_[start] == '\t')) {
        ++start;
      }
      if (start < buffer_.size() && buffer_[start] == '{') {
        const auto parsed = nlohmann::json::parse(buffer_.substr(start));
        const auto parsedOut = parseJsonCompletion(parsed);
        if (out_.content.empty() && !parsedOut.content.empty()) {
          out_.content = parsedOut.content;
          if (onDelta) onDelta(parsedOut.content);
        }
        if (out_.toolCalls.empty() && !parsedOut.toolCalls.empty()) {
          out_.toolCalls = parsedOut.toolCalls;
          int i = 0;
          for (const auto& tc : parsedOut.toolCalls) tools_[i++] = tc;
        }
        if (out_.finishReason.empty()) out_.finishReason = parsedOut.finishReason;
      }
    }
    buffer_.clear();
  }
  flushTools();
  return out_;
}

}  // namespace wikicore::llm
