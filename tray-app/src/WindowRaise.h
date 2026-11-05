#pragma once

namespace wikitray {

// Sends an EWMH _NET_ACTIVE_WINDOW request to raise and focus the given
// X11 window — the standard way to ask a window manager to bring a
// window to front (same mechanism a taskbar click uses). raylib's own
// SetWindowFocused() changes input focus but doesn't raise the window
// in stacking order on at least one real window manager — confirmed
// live. Deliberately kept out of main.cpp / not including raylib.h:
// X11's own <X11/Xlib.h> typedefs "Font" as XID, which collides with
// raylib's own Font struct typedef if both are included in the same
// translation unit.
void raiseAndFocusWindow(unsigned long x11WindowId);

}  // namespace wikitray
