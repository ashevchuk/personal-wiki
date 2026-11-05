#include "TrayIcon.h"

// No STB_IMAGE_IMPLEMENTATION here — raylib already compiles stb_image's
// implementation into libraylib.a, and wiki-tray links raylib too;
// defining it again here would duplicate every stbi_* symbol at link
// time. Just the declarations are needed; they resolve against raylib's
// already-built object code.
#include <stb_image.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>

namespace wikitray {

namespace {

using IconPixmapEntry = sdbus::Struct<int32_t, int32_t, std::vector<uint8_t>>;

// StatusNotifierItem's IconPixmap hands the host raw ARGB32 pixel data
// directly over D-Bus (network byte order: A,R,G,B per pixel) — unlike
// IconName/IconThemePath, this bypasses the host's own icon-theme
// lookup/styling pipeline entirely, which is what actually renders our
// icon correctly without a stray background plate some hosts otherwise
// draw behind a custom-theme-path icon.
std::vector<IconPixmapEntry> loadIconPixmap(const std::string& path) {
  int width = 0;
  int height = 0;
  int channels = 0;
  unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
  if (!pixels) {
    std::fprintf(stderr, "wiki-tray: failed to load icon '%s': %s\n", path.c_str(),
                 stbi_failure_reason());
    return {};
  }

  std::vector<uint8_t> argb(static_cast<size_t>(width) * height * 4);
  for (int i = 0; i < width * height; ++i) {
    argb[i * 4 + 0] = pixels[i * 4 + 3];  // A
    argb[i * 4 + 1] = pixels[i * 4 + 0];  // R
    argb[i * 4 + 2] = pixels[i * 4 + 1];  // G
    argb[i * 4 + 3] = pixels[i * 4 + 2];  // B
  }
  stbi_image_free(pixels);

  return {IconPixmapEntry{width, height, std::move(argb)}};
}

// Same next-to-the-binary / dev-source-tree fallback as
// ChatWindow.cpp's findResourcePath() — duplicated rather than shared
// since it's ~10 lines and the two files have no other reason to
// depend on each other.
std::string findResourcesDir() {
  char exePath[4096];
  const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
  if (len > 0) {
    exePath[len] = '\0';
    const std::filesystem::path candidate =
        std::filesystem::path(exePath).parent_path() / "resources";
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }
#ifdef WIKI_TRAY_SOURCE_RESOURCES_DIR
  return WIKI_TRAY_SOURCE_RESOURCES_DIR;
#else
  return "resources";
#endif
}

constexpr const char* kObjectPath = "/StatusNotifierItem";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";

constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";
constexpr int32_t kShowItemId = 1;
constexpr int32_t kQuitItemId = 2;

const char* menuLabel(int32_t id) {
  if (id == kShowItemId) return "Show window";
  if (id == kQuitItemId) return "Quit";
  return nullptr;
}

// com.canonical.dbusmenu's GetLayout is recursively typed (each item
// carries an array of further items as variants), but this menu is only
// ever one level deep — a single "Quit" item — so a concrete, non-
// recursive alias is enough; no need to model the general case.
using MenuItem = sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>,
                                std::vector<sdbus::Variant>>;
}  // namespace

TrayIcon::TrayIcon(std::string serviceName) {
  // Requesting this name IS the single-instance check: a second process
  // asking for a name the first already holds gets an sdbus::Error here,
  // before anything else about this app starts.
  connection_ = sdbus::createSessionBusConnection(sdbus::ServiceName{serviceName});

  const std::string iconPath =
      findResourcesDir() + "/icons/hicolor/256x256/apps/wiki-tray.png";
  std::vector<IconPixmapEntry> iconPixmap = loadIconPixmap(iconPath);

  object_ = sdbus::createObject(*connection_, sdbus::ObjectPath{kObjectPath});
  object_->addVTable(
      sdbus::registerMethod("Activate")
          .implementedAs([this](int32_t, int32_t) { if (onActivate_) onActivate_(); }),
      sdbus::registerMethod("SecondaryActivate")
          .implementedAs([this](int32_t, int32_t) { if (onActivate_) onActivate_(); }),
      sdbus::registerMethod("ContextMenu")
          .implementedAs([](int32_t, int32_t) {
            // No-op: the "Menu" property below already points the host
            // straight at the dbusmenu object, which is how KDE's tray
            // actually shows a right-click menu — this method is only a
            // fallback for hosts that don't use Menu, and this app sets
            // Menu, so there's nothing for it to do.
          }),
      sdbus::registerProperty("Category").withGetter([] { return std::string("ApplicationStatus"); }),
      sdbus::registerProperty("Id").withGetter([] { return std::string("wiki-tray"); }),
      sdbus::registerProperty("Title").withGetter([] { return std::string("Wiki Tray"); }),
      sdbus::registerProperty("Status").withGetter([] { return std::string("Active"); }),
      sdbus::registerProperty("WindowId").withGetter([] { return int32_t{0}; }),
      // This Plasma panel draws a white contrast plate behind the icon
      // regardless of delivery mechanism (IconThemePath, raw
      // IconPixmap bytes) or naming convention ("-symbolic" suffix) —
      // confirmed live across all three; it's how this theme renders
      // any app icon it doesn't recognize from its own installed
      // icon-theme set, not something fixable from this side without
      // actually installing the icon into the system's real icon cache.
      sdbus::registerProperty("IconName").withGetter([] { return std::string("wiki-tray"); }),
      sdbus::registerProperty("IconThemePath")
          .withGetter([] { return findResourcesDir() + "/icons"; }),
      sdbus::registerProperty("IconPixmap")
          .withGetter([iconPixmap] { return iconPixmap; }),
      sdbus::registerProperty("ItemIsMenu").withGetter([] { return false; }),
      sdbus::registerProperty("Menu").withGetter([] { return sdbus::ObjectPath{kMenuPath}; }))
      .forInterface(sdbus::InterfaceName{kItemInterface});

  // Minimal com.canonical.dbusmenu backend: "Show window" and "Quit" items, nothing
  // else. GetGroupProperties/GetProperty/AboutToShow are included for
  // hosts that call them defensively even when GetLayout already has
  // everything — cheap to answer correctly rather than leave unhandled.
  // Runs on sdbus-c++'s event-loop thread — only flips an atomic or calls
  // the activate callback, which itself just sets an atomic in main().
  const auto onMenuClicked = [this](int32_t id) {
    if (id == kShowItemId) {
      if (onActivate_) onActivate_();
    } else if (id == kQuitItemId) {
      quitRequested_ = true;
    }
  };

  menuObject_ = sdbus::createObject(*connection_, sdbus::ObjectPath{kMenuPath});
  menuObject_->addVTable(
      sdbus::registerMethod("GetLayout")
          .implementedAs([](int32_t, int32_t,
                             std::vector<std::string>) -> std::tuple<uint32_t, MenuItem> {
            auto item = [](int32_t id) {
              return MenuItem{id, {{"label", sdbus::Variant(std::string(menuLabel(id)))}}, {}};
            };
            // Without children-display=submenu on the root, Plasma fetches the
            // layout but never shows a popup — every working app's root
            // carries it (confirmed by diffing against Telegram/KGpg).
            MenuItem root{0,
                          {{"children-display", sdbus::Variant(std::string("submenu"))}},
                          {sdbus::Variant(item(kShowItemId)), sdbus::Variant(item(kQuitItemId))}};
            return std::tuple<uint32_t, MenuItem>{1u, root};
          }),
      sdbus::registerMethod("GetGroupProperties")
          .implementedAs(
              [](std::vector<int32_t> ids,
                 std::vector<std::string>) -> std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> {
                std::vector<sdbus::Struct<int32_t, std::map<std::string, sdbus::Variant>>> result;
                for (int32_t id : ids) {
                  std::map<std::string, sdbus::Variant> props;
                  if (const char* label = menuLabel(id)) {
                    props["label"] = sdbus::Variant(std::string(label));
                  }
                  result.push_back({id, props});
                }
                return result;
              }),
      sdbus::registerMethod("GetProperty")
          .implementedAs([](int32_t id, std::string name) -> sdbus::Variant {
            if (const char* label = menuLabel(id); label && name == "label") {
              return sdbus::Variant(std::string(label));
            }
            return sdbus::Variant(std::string());
          }),
      sdbus::registerMethod("AboutToShow").implementedAs([](int32_t) { return false; }),
      sdbus::registerMethod("AboutToShowGroup")
          .implementedAs([](std::vector<int32_t>) {
            return std::tuple<std::vector<int32_t>, std::vector<int32_t>>{};
          }),
      sdbus::registerMethod("EventGroup")
          .implementedAs(
              [onMenuClicked](std::vector<sdbus::Struct<int32_t, std::string, sdbus::Variant, uint32_t>> events) {
                for (const auto& e : events) {
                  if (std::get<1>(e) == "clicked") onMenuClicked(std::get<0>(e));
                }
                return std::vector<int32_t>{};
              }),
      sdbus::registerProperty("IconThemePath").withGetter([] { return std::vector<std::string>{}; }),
      sdbus::registerMethod("Event")
          .implementedAs([onMenuClicked](int32_t id, std::string eventId, sdbus::Variant,
                                 uint32_t) {
            if (eventId == "clicked") onMenuClicked(id);
          }),
      sdbus::registerProperty("Version").withGetter([] { return uint32_t{3}; }),
      sdbus::registerProperty("Status").withGetter([] { return std::string("normal"); }),
      sdbus::registerProperty("TextDirection").withGetter([] { return std::string("ltr"); }))
      .forInterface(sdbus::InterfaceName{kMenuInterface});

  // The fixed name above is the single-instance guard; Plasma's tray host
  // also expects the conventional org.kde.StatusNotifierItem-<pid>-1 name
  // (what every working SNI app registers under — acoff, Qt apps), so
  // register the item with the watcher under that one.
  const std::string sniName =
      "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
  connection_->requestName(sdbus::ServiceName{sniName});

  connection_->enterEventLoopAsync();

  auto watcherProxy =
      sdbus::createProxy(*connection_, sdbus::ServiceName{kWatcherService},
                          sdbus::ObjectPath{kWatcherPath});
  watcherProxy->callMethod("RegisterStatusNotifierItem")
      .onInterface(kWatcherService)
      .withArguments(sniName);
}

TrayIcon::~TrayIcon() = default;

void TrayIcon::setOnActivate(std::function<void()> callback) {
  onActivate_ = std::move(callback);
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
