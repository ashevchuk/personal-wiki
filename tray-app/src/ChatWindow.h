#pragma once

#include "AgentClient.h"

#include <raylib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wikitray {

struct ChatMessage {
  bool fromUser;
  std::string text;
};

// Owns one conversation's worth of UI state. A fresh AgentClient is
// created each time onShow() runs (window goes hidden -> visible after a
// full close) — conversations are deliberately ephemeral, per the design
// doc. Network calls run on a detached background thread so a slow
// agent reply never freezes the window; results are handed back through
// a shared, mutex-guarded SendState so a stale send from an abandoned
// conversation can never write into the current one's state.
class ChatWindow {
 public:
  static constexpr int kWidth = 480;
  static constexpr int kHeight = 640;
  static constexpr int kMinWidth = 320;
  static constexpr int kMinHeight = 300;

  ChatWindow(std::string serverUrl, std::string token);
  ~ChatWindow();

  void onShow();

  // Draws and processes input for one frame — call between
  // BeginDrawing()/EndDrawing().
  void update();

 private:
  struct SendState {
    std::mutex mutex;
    bool ready = false;
    AgentResult result;
  };

  void startSend(const std::string& text);
  void drawHistory(int x, int y, int width, int height);
  void pollClipboard();
  std::string clipboardLabel(int maxWidth) const;

  Font font_;

  std::string serverUrl_;
  std::string token_;
  std::shared_ptr<AgentClient> client_;
  std::shared_ptr<SendState> pendingSend_;

  std::vector<ChatMessage> history_;
  char inputBuffer_[1024] = {0};
  float scrollOffset_ = 0.0f;

  bool clipboardOfferShown_ = false;
  std::string clipboardOfferText_;
  bool clipboardIncluded_ = false;
  int clipboardPollCounter_ = 0;

  std::string lastError_;
};

}  // namespace wikitray
