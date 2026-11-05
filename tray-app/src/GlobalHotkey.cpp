#include "GlobalHotkey.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace wikitray {

namespace {

// X11 error handling is process-global (no user-data parameter), so the
// grab collision check below uses a thread-local flag a scoped handler
// sets, restoring whatever handler was installed before it.
thread_local bool g_grabFailed = false;

int grabErrorHandler(Display*, XErrorEvent* event) {
  if (event->error_code == BadAccess || event->error_code == BadValue) {
    g_grabFailed = true;
  }
  return 0;
}

struct ParsedBinding {
  unsigned int modifiers = 0;
  KeySym keysym = NoSymbol;
};

ParsedBinding parseBinding(const std::string& binding) {
  std::vector<std::string> tokens;
  std::stringstream ss(binding);
  std::string token;
  while (std::getline(ss, token, '+')) {
    tokens.push_back(token);
  }
  if (tokens.empty()) {
    throw std::runtime_error("GlobalHotkey: empty binding");
  }

  ParsedBinding parsed;
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    const std::string& mod = tokens[i];
    if (mod == "Ctrl" || mod == "Control") {
      parsed.modifiers |= ControlMask;
    } else if (mod == "Alt") {
      parsed.modifiers |= Mod1Mask;
    } else if (mod == "Shift") {
      parsed.modifiers |= ShiftMask;
    } else if (mod == "Super" || mod == "Meta" || mod == "Win") {
      parsed.modifiers |= Mod4Mask;
    } else {
      throw std::runtime_error("GlobalHotkey: unknown modifier '" + mod +
                                "' in binding '" + binding + "'");
    }
  }

  std::string keyName = tokens.back();
  if (keyName.size() == 1) {
    keyName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(keyName[0])));
  }
  parsed.keysym = XStringToKeysym(keyName.c_str());
  if (parsed.keysym == NoSymbol) {
    throw std::runtime_error("GlobalHotkey: unknown key '" + keyName +
                              "' in binding '" + binding + "'");
  }
  return parsed;
}

}  // namespace

GlobalHotkey::GlobalHotkey(const std::string& binding) : binding_(binding) {
  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    throw std::runtime_error("GlobalHotkey: XOpenDisplay failed");
  }

  const ParsedBinding parsed = parseBinding(binding);
  modifiers_ = parsed.modifiers;
  keycode_ = XKeysymToKeycode(display_, parsed.keysym);
  if (keycode_ == 0) {
    XCloseDisplay(display_);
    display_ = nullptr;
    throw std::runtime_error("GlobalHotkey: key has no keycode on this keyboard");
  }

  const Window root = DefaultRootWindow(display_);

  // A passive XGrabKey only matches the EXACT modifier state given — so
  // without also grabbing every combination with NumLock/CapsLock/
  // ScrollLock added, the hotkey silently stops firing the moment any of
  // those is toggled on. Grab all 8 combinations of those three locks.
  constexpr unsigned int kNumLockMask = Mod2Mask;
  constexpr unsigned int kScrollLockMask = Mod5Mask;
  const unsigned int lockCombos[] = {0,
                                      LockMask,
                                      kNumLockMask,
                                      kScrollLockMask,
                                      LockMask | kNumLockMask,
                                      LockMask | kScrollLockMask,
                                      kNumLockMask | kScrollLockMask,
                                      LockMask | kNumLockMask | kScrollLockMask};

  auto* previousHandler = XSetErrorHandler(grabErrorHandler);
  g_grabFailed = false;

  std::vector<unsigned int> grabbedCombos;
  for (unsigned int lockBits : lockCombos) {
    XGrabKey(display_, keycode_, modifiers_ | lockBits, root, False,
              GrabModeAsync, GrabModeAsync);
    XSync(display_, False);
    if (g_grabFailed) {
      break;
    }
    grabbedCombos.push_back(modifiers_ | lockBits);
  }

  XSetErrorHandler(previousHandler);

  if (g_grabFailed || grabbedCombos.size() != 8) {
    // Partial grab is worse than none — release whatever did succeed so
    // this process holds no stray grabs.
    for (unsigned int combo : grabbedCombos) {
      XUngrabKey(display_, keycode_, combo, root);
    }
    grabbed_ = false;
    return;
  }

  XSelectInput(display_, root, KeyPressMask);
  grabbed_ = true;
}

GlobalHotkey::~GlobalHotkey() {
  if (display_) {
    if (grabbed_) {
      const Window root = DefaultRootWindow(display_);
      XUngrabKey(display_, AnyKey, AnyModifier, root);
    }
    XCloseDisplay(display_);
  }
}

void GlobalHotkey::poll(const std::function<void()>& onPressed) {
  if (!grabbed_ || !display_) {
    return;
  }
  while (XPending(display_) > 0) {
    XEvent event;
    XNextEvent(display_, &event);
    if (event.type == KeyPress && event.xkey.keycode == static_cast<unsigned int>(keycode_)) {
      if (onPressed) {
        onPressed();
      }
    }
  }
}

}  // namespace wikitray
