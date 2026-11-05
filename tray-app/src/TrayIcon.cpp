#include "TrayIcon.h"

namespace wikitray {

namespace {
constexpr const char* kObjectPath = "/StatusNotifierItem";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
}  // namespace

TrayIcon::TrayIcon(std::string serviceName) {
  // Requesting this name IS the single-instance check: a second process
  // asking for a name the first already holds gets an sdbus::Error here,
  // before anything else about this app starts.
  connection_ = sdbus::createSessionBusConnection(sdbus::ServiceName{serviceName});

  object_ = sdbus::createObject(*connection_, sdbus::ObjectPath{kObjectPath});
  object_->addVTable(
      sdbus::registerMethod("Activate")
          .implementedAs([this](int32_t, int32_t) { if (onActivate_) onActivate_(); }),
      sdbus::registerMethod("SecondaryActivate")
          .implementedAs([this](int32_t, int32_t) { if (onActivate_) onActivate_(); }),
      sdbus::registerMethod("ContextMenu")
          .implementedAs([](int32_t, int32_t) {
            // No com.canonical.dbusmenu backend yet (deferred to M5) —
            // a no-op here is correct: the tray host just won't show a
            // context menu until that's wired up.
          }),
      sdbus::registerProperty("Category").withGetter([] { return std::string("ApplicationStatus"); }),
      sdbus::registerProperty("Id").withGetter([] { return std::string("wiki-tray"); }),
      sdbus::registerProperty("Title").withGetter([] { return std::string("Wiki Tray"); }),
      sdbus::registerProperty("Status").withGetter([] { return std::string("Active"); }),
      sdbus::registerProperty("WindowId").withGetter([] { return uint32_t{0}; }),
      sdbus::registerProperty("IconName").withGetter([] { return std::string("accessories-text-editor"); }),
      sdbus::registerProperty("ItemIsMenu").withGetter([] { return false; }))
      .forInterface(sdbus::InterfaceName{kItemInterface});

  connection_->enterEventLoopAsync();

  auto watcherProxy =
      sdbus::createProxy(*connection_, sdbus::ServiceName{kWatcherService},
                          sdbus::ObjectPath{kWatcherPath});
  watcherProxy->callMethod("RegisterStatusNotifierItem")
      .onInterface(kWatcherService)
      .withArguments(serviceName);
}

TrayIcon::~TrayIcon() = default;

void TrayIcon::setOnActivate(std::function<void()> callback) {
  onActivate_ = std::move(callback);
}

void TrayIcon::setOnQuit(std::function<void()> callback) {
  onQuit_ = std::move(callback);
}

void TrayIcon::notify(const std::string& summary, const std::string& body) {
  auto proxy = sdbus::createProxy(*connection_,
                                   sdbus::ServiceName{"org.freedesktop.Notifications"},
                                   sdbus::ObjectPath{"/org/freedesktop/Notifications"});
  proxy->callMethod("Notify")
      .onInterface("org.freedesktop.Notifications")
      .withArguments(std::string("wiki-tray"), uint32_t{0}, std::string(""),
                      summary, body, std::vector<std::string>{},
                      std::map<std::string, sdbus::Variant>{}, int32_t{-1});
}

}  // namespace wikitray
