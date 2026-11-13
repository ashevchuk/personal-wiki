#pragma once

struct _XDisplay;

namespace wikitray {

// Lets the main loop sleep with zero wakeups while the window is hidden,
// instead of spinning at frame rate. wait() blocks until GLFW's X11
// connection or the hotkey's X11 connection has input, or until wake() is
// called (from the tray's D-Bus thread, which has no X11 fd to poll).
class IdleWait {
 public:
  IdleWait();
  ~IdleWait();

  IdleWait(const IdleWait&) = delete;
  IdleWait& operator=(const IdleWait&) = delete;

  // Thread-safe. A wake() that happens before wait() is not lost: the
  // next wait() returns immediately.
  void wake();

  // hotkeyDisplay may be null (hotkey not grabbed). Requires the raylib
  // window (and therefore GLFW's X11 connection) to exist.
  void wait(struct _XDisplay* hotkeyDisplay);

 private:
  int eventFd_ = -1;
};

}  // namespace wikitray
