#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace wikitray {

// Registers org.kde.StatusNotifierItem over D-Bus and owns the well-known
// bus name that doubles as single-instance enforcement: requesting it on
// the session bus throws if another wiki-tray process already holds it.
// Also registers a minimal com.canonical.dbusmenu object (one "Quit"
// item) and advertises it via the item's own Menu property, so a
// right-click shows a real context menu. D-Bus callbacks (Activate, the
// menu's Quit) run on sdbus-c++'s own event-loop thread, not the
// caller's — they only flip a std::atomic or the callbacks passed in,
// which the main loop is responsible for picking up safely.
class TrayIcon {
 public:
  // The callbacks are taken here, not via setters, because the D-Bus
  // thread starts inside the constructor and may invoke them at once.
  // onQuit runs right after quitRequested() becomes true.
  TrayIcon(std::string serviceName, std::function<void()> onActivate,
           std::function<void()> onQuit);
  ~TrayIcon();

  // True once the context menu's Quit item has been clicked. Safe to
  // poll from the main loop every frame.
  bool quitRequested() const { return quitRequested_; }

  // org.freedesktop.Notifications.Notify — reused by GlobalHotkey's
  // grab-collision message so it's a real desktop notification, not
  // something only visible by hovering the tray icon.
  void notify(const std::string& summary, const std::string& body);

 private:
  std::unique_ptr<sdbus::IConnection> connection_;
  std::unique_ptr<sdbus::IObject> object_;
  std::unique_ptr<sdbus::IObject> menuObject_;
  const std::function<void()> onActivate_;
  const std::function<void()> onQuit_;
  std::atomic<bool> quitRequested_{false};
};

}  // namespace wikitray
