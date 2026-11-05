#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <functional>
#include <memory>
#include <string>

namespace wikitray {

// Registers org.kde.StatusNotifierItem over D-Bus and owns the well-known
// bus name that doubles as single-instance enforcement: requesting it on
// the session bus throws if another wiki-tray process already holds it.
// D-Bus callbacks (Activate, Quit) run on sdbus-c++'s own event-loop
// thread, not the caller's thread — they only flip the callbacks passed
// in, which the main loop is responsible for picking up safely.
class TrayIcon {
 public:
  explicit TrayIcon(std::string serviceName);
  ~TrayIcon();

  void setOnActivate(std::function<void()> callback);
  void setOnQuit(std::function<void()> callback);

  // org.freedesktop.Notifications.Notify — reused by GlobalHotkey's
  // grab-collision message so it's a real desktop notification, not
  // something only visible by hovering the tray icon.
  void notify(const std::string& summary, const std::string& body);

 private:
  std::unique_ptr<sdbus::IConnection> connection_;
  std::unique_ptr<sdbus::IObject> object_;
  std::function<void()> onActivate_;
  std::function<void()> onQuit_;
};

}  // namespace wikitray
