#include "IdleWait.h"

// Kept out of any TU that includes raylib.h: Xlib's `Font`/`Cursor`
// typedefs collide with raylib's own types of the same names.
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xlib.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>

namespace wikitray {

namespace {
// poll() only sees bytes still in the socket; events Xlib has already read
// into its own queue (e.g. while waiting for some unrelated reply) are
// invisible to it, so blocking with those pending would stall them.
bool hasQueuedEvents(Display* display) {
  return display && XEventsQueued(display, QueuedAfterFlush) > 0;
}
}  // namespace

IdleWait::IdleWait() : eventFd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (eventFd_ < 0) {
    throw std::runtime_error("eventfd() failed");
  }
}

IdleWait::~IdleWait() {
  close(eventFd_);
}

void IdleWait::wake() {
  const std::uint64_t one = 1;
  // Can only fail with EAGAIN when the counter is saturated, which still
  // leaves the fd readable — the wakeup is delivered either way.
  [[maybe_unused]] const ssize_t written = write(eventFd_, &one, sizeof one);
}

void IdleWait::wait(Display* hotkeyDisplay) {
  Display* glfwDisplay = glfwGetX11Display();
  if (hasQueuedEvents(glfwDisplay) || hasQueuedEvents(hotkeyDisplay)) {
    return;
  }

  pollfd fds[3];
  nfds_t count = 0;
  fds[count++] = {eventFd_, POLLIN, 0};
  fds[count++] = {ConnectionNumber(glfwDisplay), POLLIN, 0};
  if (hotkeyDisplay) {
    fds[count++] = {ConnectionNumber(hotkeyDisplay), POLLIN, 0};
  }

  // EINTR just means "go round the main loop once more", same as a wakeup.
  poll(fds, count, -1);

  std::uint64_t drained = 0;
  [[maybe_unused]] const ssize_t readBytes = read(eventFd_, &drained, sizeof drained);
}

}  // namespace wikitray
