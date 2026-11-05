#include "WindowRaise.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace wikitray {

namespace {

// Window managers with focus-stealing prevention (KWin included) weigh
// a _NET_ACTIVE_WINDOW request's timestamp to decide whether to trust
// it — passing the literal CurrentTime (0) is exactly what an
// untrusted/stale request looks like and gets silently ignored,
// confirmed live (the message sent correctly, no error, but focus
// never moved). The standard workaround, same technique GTK/Qt use
// internally: trigger a property change on our own window and read the
// timestamp the server attaches to the resulting PropertyNotify —
// that's a genuine, current server timestamp. Bounded by a handful of
// XNextEvent calls so a server that never delivers this (shouldn't
// happen for a local round-trip) can't hang the caller forever.
Time currentServerTime(Display* display, Window window) {
  const Atom scratchProp = XInternAtom(display, "_WIKI_TRAY_TIMESTAMP", False);
  XSelectInput(display, window, PropertyChangeMask);
  unsigned char dummy = 0;
  XChangeProperty(display, window, scratchProp, XA_STRING, 8, PropModeReplace, &dummy, 1);

  for (int attempt = 0; attempt < 20; ++attempt) {
    XEvent event;
    XNextEvent(display, &event);
    if (event.type == PropertyNotify && event.xproperty.window == window &&
        event.xproperty.atom == scratchProp) {
      return event.xproperty.time;
    }
  }
  return CurrentTime;  // fallback — the request may just get ignored
}

}  // namespace

void raiseAndFocusWindow(unsigned long x11WindowId) {
  Display* display = XOpenDisplay(nullptr);
  if (!display) {
    return;
  }

  const Window window = static_cast<Window>(x11WindowId);
  const Time timestamp = currentServerTime(display, window);

  const Atom netActiveWindow = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.window = window;
  event.xclient.message_type = netActiveWindow;
  event.xclient.format = 32;
  event.xclient.data.l[0] = 1;  // source indication: normal application
  event.xclient.data.l[1] = timestamp;
  event.xclient.data.l[2] = 0;

  const Window root = DefaultRootWindow(display);
  XSendEvent(display, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
  XFlush(display);
  XCloseDisplay(display);
}

}  // namespace wikitray
