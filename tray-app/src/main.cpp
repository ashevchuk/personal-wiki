// wiki-tray: standalone KDE/X11 system-tray client for the wiki's
// POST /mcp/agent endpoint. See /home/slayer/devel/wiki/.notes/tray-app-design.md.

#include "ChatWindow.h"
#include "Config.h"
#include "GlobalHotkey.h"
#include "TrayIcon.h"

#include <raylib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

namespace {
constexpr const char* kTrayServiceName = "org.wikitray.TrayIcon";
// 60, not 30: polling-based input (IsKeyPressed/GetCharPressed) only
// catches a key transition that's still "down" at the moment we poll —
// a very fast transition that completes entirely between two polls is
// invisible. 60fps halves that blind window; CPU stays negligible
// either way since the loop spends virtually all its time asleep.
constexpr int kTargetFps = 60;
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
    std::puts(WIKI_TRAY_VERSION);
    return 0;
  }

  wikitray::Config cfg = wikitray::Config::load();

  // Requesting this bus name IS the single-instance check — a second
  // process throws here if one is already running.
  wikitray::TrayIcon tray(kTrayServiceName);

  std::atomic<bool> showRequested{false};
  tray.setOnActivate([&showRequested] { showRequested = true; });

  wikitray::GlobalHotkey hotkey(cfg.hotkey);
  if (!hotkey.grabbed()) {
    tray.notify("wiki-tray",
                "Hotkey '" + cfg.hotkey +
                    "' could not be registered — it's already bound to "
                    "something else. Use the tray icon instead.");
  }

  SetTraceLogLevel(LOG_WARNING);
  // No FLAG_MSAA_4X_HINT, ever: on the NVIDIA driver, interactively
  // resizing a multisampled window triggers Xid 69 GPU errors that
  // stall the compositor (see .notes/tray-app-design.md's M2 entry,
  // same root cause fixed in /home/slayer/devel/alarmmap's d530849).
  // This window has a fixed pixel layout and isn't resizable anyway.
  InitWindow(wikitray::ChatWindow::kWidth, wikitray::ChatWindow::kHeight, "Wiki Tray");
  SetWindowState(FLAG_WINDOW_HIDDEN);  // starts hidden; shown on hotkey/tray click

  wikitray::ChatWindow chatWindow(cfg.serverUrl, cfg.token);
  bool visible = false;

  const std::function<void()> onHotkeyOrActivate = [&showRequested] {
    showRequested = true;
  };

  // Manual frame control throughout: this vcpkg build of raylib has both
  // SUPPORT_CUSTOM_FRAME_CONTROL and SUPPORT_BUSY_WAIT_LOOP baked on (a
  // vcpkg config-parsing bug — see .notes/tray-app-design.md's M2
  // entry), so EndDrawing() never auto-swaps/paces/polls, and raylib's
  // own WaitTime() busy-spins a full CPU core instead of sleeping.
  // PollInputEvents()/SwapScreenBuffer() are called explicitly below,
  // and std::this_thread::sleep_for (never WaitTime()) does the pacing.
  double previousTime = GetTime();
  bool running = true;

  while (running) {
    hotkey.poll(onHotkeyOrActivate);
    PollInputEvents();

    if (showRequested.exchange(false)) {
      if (!visible) {
        chatWindow.onShow();
      }
      ClearWindowState(FLAG_WINDOW_HIDDEN);
      visible = true;
    }

    // WindowShouldClose() covers both the OS close button and Escape;
    // either just hides the window back to the tray, it never quits the
    // app — only ChatWindow's own Quit button does that. raylib resets
    // the underlying flag on every PollInputEvents() call, so this is
    // naturally a one-shot check, not sticky state.
    if (visible && WindowShouldClose()) {
      SetWindowState(FLAG_WINDOW_HIDDEN);
      visible = false;
    }

    if (visible) {
      BeginDrawing();
      bool quitRequested = false;
      chatWindow.update(&quitRequested);
      EndDrawing();
      if (quitRequested) {
        running = false;
      }
    }

    SwapScreenBuffer();
    double currentTime = GetTime();
    const double updateDrawTime = currentTime - previousTime;
    const double waitTime = (1.0 / kTargetFps) - updateDrawTime;
    if (waitTime > 0.0) {
      std::this_thread::sleep_for(std::chrono::duration<double>(waitTime));
      currentTime = GetTime();
    }
    previousTime = currentTime;
  }

  CloseWindow();
  return 0;
}
