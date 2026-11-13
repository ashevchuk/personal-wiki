#pragma once

#include <functional>
#include <string>

struct _XDisplay;

namespace wikitray {

// Grabs a global hotkey (e.g. "Ctrl+Alt+W") on its own X11 connection,
// deliberately separate from raylib/GLFW's own so this never fights it
// for the display handle. Call poll() every frame from the main loop —
// non-blocking, invokes the callback if the hotkey fired since the last
// call.
class GlobalHotkey {
 public:
  explicit GlobalHotkey(const std::string& binding);
  ~GlobalHotkey();

  GlobalHotkey(const GlobalHotkey&) = delete;
  GlobalHotkey& operator=(const GlobalHotkey&) = delete;

  // False if the combo was already grabbed by something else on the
  // desktop (collision) — the caller stays usable (reachable by tray
  // click), just without this hotkey, per the design doc's anti-goal
  // about failing loudly instead of silently.
  bool grabbed() const { return grabbed_; }
  const std::string& binding() const { return binding_; }

  void poll(const std::function<void()>& onPressed);

  // The hotkey's own X11 connection (null if it couldn't be opened), so
  // the main loop can block on it while the window is hidden.
  struct _XDisplay* display() const { return display_; }

 private:
  struct _XDisplay* display_ = nullptr;
  unsigned int modifiers_ = 0;
  int keycode_ = 0;
  bool grabbed_ = false;
  std::string binding_;
};

}  // namespace wikitray
