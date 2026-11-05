// wiki-tray: standalone KDE/X11 system-tray client for the wiki's
// POST /mcp/agent endpoint. See /home/slayer/devel/wiki/.notes/tray-app-design.md.

#include "ChatWindow.h"
#include "Config.h"
#include "GlobalHotkey.h"
#include "TrayIcon.h"

#include "WindowRaise.h"

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
  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(wikitray::ChatWindow::kWidth, wikitray::ChatWindow::kHeight, "Wiki Tray");
  SetWindowMinSize(wikitray::ChatWindow::kMinWidth, wikitray::ChatWindow::kMinHeight);
  SetWindowState(FLAG_WINDOW_HIDDEN);  // starts hidden; shown on hotkey/tray click

  // Scoped so ~ChatWindow() (which calls UnloadFont/UnloadTexture) runs
  // BEFORE CloseWindow() below — a local variable at main()'s own scope
  // would be destructed AFTER CloseWindow() already tore down the GL
  // context, since C++ destroys locals in reverse order after the final
  // `return`, not at the point of the CloseWindow() statement. Calling
  // GL functions against an already-destroyed context segfaults;
  // confirmed live via a real crash on Quit, backtrace through
  // ~ChatWindow -> UnloadFont -> UnloadTexture -> rlUnloadTexture.
  {
    wikitray::ChatWindow chatWindow(cfg.serverUrl, cfg.token);
    bool visible = false;

    const std::function<void()> onHotkeyOrActivate = [&showRequested] {
      showRequested = true;
    };

    // Manual frame control throughout: this vcpkg build of raylib has
    // both SUPPORT_CUSTOM_FRAME_CONTROL and SUPPORT_BUSY_WAIT_LOOP baked
    // on (a vcpkg config-parsing bug — see .notes/tray-app-design.md's
    // M2 entry), so EndDrawing() never auto-swaps/paces/polls, and
    // raylib's own WaitTime() busy-spins a full CPU core instead of
    // sleeping. PollInputEvents()/SwapScreenBuffer() are called
    // explicitly below, and std::this_thread::sleep_for (never
    // WaitTime()) does the pacing.
    double previousTime = GetTime();
    double raiseAgainAt = 0.0;
    bool running = true;

    while (running) {
      hotkey.poll(onHotkeyOrActivate);
      PollInputEvents();

      if (showRequested.exchange(false)) {
        if (!visible) {
          // Unhide (and pump once more so the window is actually mapped)
          // BEFORE reading the clipboard: an unmapped window doesn't
          // reliably receive the X11 SelectionNotify a clipboard read
          // waits on, so GetClipboardText() falls back to a stale
          // cached value if asked while still hidden — confirmed live,
          // this order is required, not cosmetic.
          ClearWindowState(FLAG_WINDOW_HIDDEN);
          PollInputEvents();
          chatWindow.onShow();
        } else {
          ClearWindowState(FLAG_WINDOW_HIDDEN);
        }
        // Unhiding alone doesn't reliably raise/focus the window on
        // this window manager — a hotkey/tray activation could leave
        // it mapped but still behind other windows, unfocused. Restore
        // (in case it was minimized) and send a real EWMH activation
        // request every time it's shown.
        if (IsWindowMinimized()) {
          RestoreWindow();
        }
        wikitray::raiseAndFocusWindow(
            *static_cast<unsigned long*>(GetWindowHandle()));
        // Activating from the tray menu: KWin hands focus back to the
        // previously active window when the popup closes, which happens
        // just after this runs — raise once more shortly afterwards.
        raiseAgainAt = GetTime() + 0.25;
        visible = true;
      }

      if (raiseAgainAt > 0.0 && GetTime() >= raiseAgainAt) {
        raiseAgainAt = 0.0;
        if (visible) {
          wikitray::raiseAndFocusWindow(
              *static_cast<unsigned long*>(GetWindowHandle()));
        }
      }

      // WindowShouldClose() covers both the OS close button and Escape;
      // either just hides the window back to the tray, it never quits
      // the app — only the tray menu's Quit item does that. raylib resets
      // the underlying flag on every PollInputEvents() call, so this is
      // naturally a one-shot check, not sticky state.
      if (visible && WindowShouldClose()) {
        SetWindowState(FLAG_WINDOW_HIDDEN);
        visible = false;
      }

      if (visible) {
        BeginDrawing();
        chatWindow.update();
        EndDrawing();
      }

      if (tray.quitRequested()) {
        running = false;
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
  }  // chatWindow's scope — destructed here, GL context still alive

  CloseWindow();
  return 0;
}
