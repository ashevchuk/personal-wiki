#include "ChatWindow.h"

#include <raylib.h>
// raygui is a single-header STB-style library — the implementation must
// be defined in exactly one translation unit; this is that one.
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

#include <unistd.h>

#include <filesystem>
#include <thread>
#include <vector>

namespace wikitray {

namespace {
constexpr int kWindowWidth = ChatWindow::kWidth;
constexpr int kWindowHeight = ChatWindow::kHeight;
constexpr int kPadding = 10;
constexpr int kInputHeight = 30;
constexpr int kButtonWidth = 70;

// Terminal green-on-black palette.
constexpr Color kBg{8, 8, 8, 255};
constexpr Color kGreen{51, 255, 68, 255};
constexpr Color kGreenDim{0, 160, 32, 255};
constexpr Color kAmberError{255, 176, 0, 255};

// Finds a bundled resource (next to the running binary, for an installed/
// AppImage layout; falling back to the source tree's own resources/ dir
// for a dev build run straight out of tray-app/build/).
std::string findResourcePath(const std::string& name) {
  char exePath[4096];
  const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
  if (len > 0) {
    exePath[len] = '\0';
    const std::filesystem::path candidate =
        std::filesystem::path(exePath).parent_path() / "resources" / name;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }
#ifdef WIKI_TRAY_SOURCE_RESOURCES_DIR
  const std::filesystem::path devCandidate =
      std::filesystem::path(WIKI_TRAY_SOURCE_RESOURCES_DIR) / name;
  if (std::filesystem::exists(devCandidate)) {
    return devCandidate.string();
  }
#endif
  return name;
}

Font loadEmbeddedFont() {
  // ASCII + Latin-1 Supplement + Cyrillic — covers the agent's own
  // replies, which may be in Ukrainian (see .notes/tray-app-design.md:
  // raylib's own default font only has 224 glyphs and no Cyrillic at
  // all, which would render every Ukrainian reply as boxes).
  std::vector<int> codepoints;
  for (int cp = 0x20; cp <= 0xFF; ++cp) codepoints.push_back(cp);
  for (int cp = 0x400; cp <= 0x4FF; ++cp) codepoints.push_back(cp);
  // General Punctuation block: em/en dash, smart quotes, ellipsis — all
  // of which show up in plain English/Ukrainian prose (including this
  // app's own error messages).
  for (int cp = 0x2010; cp <= 0x2027; ++cp) codepoints.push_back(cp);

  const std::string path = findResourcePath("DejaVuSansMono.ttf");
  Font font = LoadFontEx(path.c_str(), 20, codepoints.data(),
                          static_cast<int>(codepoints.size()));
  if (font.texture.id == 0) {
    // Embedded font genuinely missing — fall back rather than crash, but
    // this means Cyrillic text will render as boxes again.
    return GetFontDefault();
  }
  SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
  return font;
}

void applyTerminalStyle() {
  const int bg = ColorToInt(kBg);
  const int green = ColorToInt(kGreen);
  const int greenDim = ColorToInt(kGreenDim);

  for (int control : {DEFAULT, BUTTON, TEXTBOX, LABEL}) {
    GuiSetStyle(control, BASE_COLOR_NORMAL, bg);
    GuiSetStyle(control, BASE_COLOR_FOCUSED, bg);
    // raygui draws a TEXTBOX in STATE_PRESSED for its whole editMode
    // (not just while the mouse button is physically down), so this
    // must default to the normal black-on-green look too, or an
    // actively-focused input box renders as a solid green block.
    GuiSetStyle(control, BASE_COLOR_PRESSED, bg);
    GuiSetStyle(control, BORDER_COLOR_NORMAL, greenDim);
    GuiSetStyle(control, BORDER_COLOR_FOCUSED, green);
    GuiSetStyle(control, BORDER_COLOR_PRESSED, green);
    GuiSetStyle(control, TEXT_COLOR_NORMAL, green);
    GuiSetStyle(control, TEXT_COLOR_FOCUSED, green);
    GuiSetStyle(control, TEXT_COLOR_PRESSED, green);
  }
  // Only a genuinely clicked BUTTON gets the inverted "cursor block"
  // look (solid green fill, dark text) — TEXTBOX's pressed state means
  // "being edited," not "clicked," so it must stay the normal style.
  GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, green);
  GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, bg);
}

const char* errorMessage(AgentError error) {
  switch (error) {
    case AgentError::Unauthorized:
      return "Auth failed — token revoked, or agent access is off for it.";
    case AgentError::ServiceUnavailable:
      return "Server has no LLM configured right now.";
    case AgentError::RateLimited:
      return "Rate limited — wait a moment and try again.";
    case AgentError::NetworkError:
      return "Couldn't reach the server — check the network.";
    case AgentError::Other:
      return "Server returned an unexpected error.";
    case AgentError::None:
      return "";
  }
  return "";
}
}  // namespace

ChatWindow::ChatWindow(std::string serverUrl, std::string token)
    : serverUrl_(std::move(serverUrl)), token_(std::move(token)) {
  font_ = loadEmbeddedFont();
  GuiSetFont(font_);
  GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
  applyTerminalStyle();
}

ChatWindow::~ChatWindow() {
  if (font_.texture.id != GetFontDefault().texture.id) {
    UnloadFont(font_);
  }
}

void ChatWindow::onShow() {
  client_ = std::make_shared<AgentClient>(serverUrl_, token_);
  pendingSend_.reset();
  history_.clear();
  inputBuffer_[0] = '\0';
  scrollOffset_ = 0.0f;
  lastError_.clear();
  clipboardIncluded_ = false;

  const char* clip = GetClipboardText();
  clipboardOfferText_ = clip ? clip : "";
  clipboardOfferShown_ = !clipboardOfferText_.empty();
}

void ChatWindow::startSend(const std::string& text) {
  std::string fullText = text;
  if (clipboardIncluded_ && !clipboardOfferText_.empty()) {
    fullText += "\n\n---\n" + clipboardOfferText_;
    clipboardIncluded_ = false;
  }

  history_.push_back({true, text});
  lastError_.clear();

  auto client = client_;
  auto state = std::make_shared<SendState>();
  pendingSend_ = state;

  std::thread([client, state, fullText] {
    AgentResult result = client->sendTurn(fullText);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->ready = true;
    state->result = std::move(result);
  }).detach();
}

void ChatWindow::drawHistory(int x, int y, int width, int height) {
  const int fontSize = 16;
  const int lineHeight = fontSize + 4;
  const int wrapWidth = width - 2 * kPadding;
  const float spacing = 1.0f;

  struct Line {
    std::string text;
    bool fromUser;
  };
  std::vector<Line> lines;
  for (const auto& msg : history_) {
    const std::string prefix = msg.fromUser ? "$ " : "# ";
    std::string remaining = msg.text;
    std::string current = prefix;
    std::string word;
    auto flushLine = [&] {
      lines.push_back({current, msg.fromUser});
      current.clear();
    };
    for (size_t i = 0; i <= remaining.size(); ++i) {
      const char c = i < remaining.size() ? remaining[i] : ' ';
      if (c == '\n' || i == remaining.size()) {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (!word.empty()) current = candidate;
        if (!current.empty()) flushLine();
        word.clear();
        continue;
      }
      if (c == ' ') {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (MeasureTextEx(font_, candidate.c_str(), static_cast<float>(fontSize), spacing).x >
                wrapWidth &&
            !current.empty()) {
          flushLine();
          current = word;
        } else {
          current = candidate;
        }
        word.clear();
      } else {
        word += c;
      }
    }
    lines.push_back({"", false});  // blank separator between messages
  }

  const int contentHeight = static_cast<int>(lines.size()) * lineHeight;
  const float maxScroll = static_cast<float>(contentHeight > height ? contentHeight - height : 0);
  scrollOffset_ -= GetMouseWheelMove() * 3 * lineHeight;
  if (scrollOffset_ < 0) scrollOffset_ = 0;
  if (scrollOffset_ > maxScroll) scrollOffset_ = maxScroll;

  BeginScissorMode(x, y, width, height);
  DrawRectangle(x, y, width, height, kBg);
  int drawY = y - static_cast<int>(scrollOffset_);
  for (const auto& line : lines) {
    if (drawY + lineHeight >= y && drawY <= y + height) {
      const Color color = line.fromUser ? kGreen : RAYWHITE;
      DrawTextEx(font_, line.text.c_str(), {static_cast<float>(x + kPadding), static_cast<float>(drawY)},
                 static_cast<float>(fontSize), spacing, color);
    }
    drawY += lineHeight;
  }
  EndScissorMode();
  DrawRectangleLines(x, y, width, height, kGreenDim);
}

void ChatWindow::update(bool* quitRequested) {
  *quitRequested = false;
  ClearBackground(kBg);

  if (GuiButton({kWindowWidth - kButtonWidth - kPadding, kPadding,
                  static_cast<float>(kButtonWidth), 24},
                 "Quit")) {
    *quitRequested = true;
  }

  int topY = kPadding + 24 + kPadding;

  if (clipboardOfferShown_) {
    std::string preview = clipboardOfferText_.substr(0, 60);
    const std::string label = "clipboard: \"" + preview + "\"...";
    GuiLabel({kPadding, static_cast<float>(topY), kWindowWidth - 2.0f * kPadding, 20}, label.c_str());
    topY += 24;
    if (GuiButton({kPadding, static_cast<float>(topY), 100, 24}, "Include")) {
      clipboardIncluded_ = true;
      clipboardOfferShown_ = false;
    }
    if (GuiButton({kPadding + 110, static_cast<float>(topY), 100, 24}, "Dismiss")) {
      clipboardOfferShown_ = false;
    }
    topY += 24 + kPadding;
  }

  const int historyBottom = kWindowHeight - kInputHeight - 2 * kPadding -
                              (lastError_.empty() ? 0 : 20);
  drawHistory(kPadding, topY, kWindowWidth - 2 * kPadding, historyBottom - topY);

  int inputY = historyBottom + kPadding;
  if (!lastError_.empty()) {
    DrawTextEx(font_, lastError_.c_str(), {static_cast<float>(kPadding), static_cast<float>(inputY)},
               14, 1.0f, kAmberError);
    inputY += 20;
  }

  const bool sending = pendingSend_ != nullptr;
  GuiTextBox({kPadding, static_cast<float>(inputY),
              kWindowWidth - 2.0f * kPadding - kButtonWidth - kPadding,
              static_cast<float>(kInputHeight)},
             inputBuffer_, sizeof(inputBuffer_), !sending);

  const bool sendClicked = GuiButton({kWindowWidth - kButtonWidth - kPadding,
                                        static_cast<float>(inputY),
                                        static_cast<float>(kButtonWidth),
                                        static_cast<float>(kInputHeight)},
                                       sending ? "..." : "Send") != 0;
  const bool enterPressed = !sending && IsKeyPressed(KEY_ENTER);

  if (!sending && (sendClicked || enterPressed) && inputBuffer_[0] != '\0') {
    startSend(inputBuffer_);
    inputBuffer_[0] = '\0';
  }

  if (pendingSend_) {
    std::lock_guard<std::mutex> lock(pendingSend_->mutex);
    if (pendingSend_->ready) {
      const AgentResult& result = pendingSend_->result;
      if (result.error != AgentError::None) {
        lastError_ = errorMessage(result.error);
      } else {
        std::string reply = result.reply;
        for (const auto& doc : result.documentsCreated) {
          reply += "\n[created: " + doc + "]";
        }
        history_.push_back({false, reply});
      }
      pendingSend_.reset();
    }
  }
}

}  // namespace wikitray
