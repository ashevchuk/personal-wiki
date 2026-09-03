// wiki-server — HTTP entrypoint (Drogon).
//
// Milestone 1: auth (argon2id + SQLite sessions), CSRF, PathGuard-backed
// read of one vault document with public/private gating. Full CRUD,
// search, and navigation land M2/M3; see
// /home/slayer/.claude/plans/zazzy-twirling-sundae.md.

#include "auth/AdminAccount.h"
#include "auth/AuthFilter.h"
#include "auth/AuthServices.h"
#include "auth/CsrfFilter.h"
#include "auth/PasswordHasher.h"
#include "auth/RateLimiter.h"
#include "auth/SessionStore.h"
#include "config/AppConfig.h"
#include "controllers/AuthRoutes.h"
#include "controllers/DocumentRoutes.h"
#include "core/wikicore.h"
#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <drogon/drogon.h>
#include <termios.h>
#include <unistd.h>

// Generated from views/EditPage.csp by drogon_ctl (see CMakeLists.txt's
// drogon_create_views call).
#include "EditPage.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

// Reads a line from stdin with terminal echo disabled — standard practice
// for CLI password prompts, avoiding a shoulder-surfed/scrollback-logged
// plaintext password.
std::string readPasswordFromTerminal(const std::string& prompt) {
  std::cout << prompt << std::flush;

  termios oldTermios{};
  const bool isTty = ::isatty(STDIN_FILENO);
  if (isTty) {
    tcgetattr(STDIN_FILENO, &oldTermios);
    termios noEcho = oldTermios;
    noEcho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &noEcho);
  }

  std::string password;
  std::getline(std::cin, password);

  if (isTty) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldTermios);
    std::cout << std::endl;
  }
  return password;
}

// `wiki-server --create-admin`: writes/overwrites the single admin row and
// exits, without starting the HTTP server. Run this once after a fresh
// install (or to reset a forgotten password), then start the server
// normally.
int runCreateAdmin(wikicore::index::Database& db) {
  std::cout << "Username: " << std::flush;
  std::string username;
  std::getline(std::cin, username);
  if (username.empty()) {
    std::cerr << "username must not be empty\n";
    return 1;
  }

  const std::string password = readPasswordFromTerminal("Password: ");
  const std::string confirm = readPasswordFromTerminal("Confirm password: ");
  if (password != confirm) {
    std::cerr << "passwords did not match\n";
    return 1;
  }
  if (password.empty()) {
    std::cerr << "password must not be empty\n";
    return 1;
  }

  const std::string encodedHash = wikicore::auth::PasswordHasher::hash(password);
  wikicore::auth::AdminAccount(db).createOrReplace(username, encodedHash);
  std::cout << "Admin account '" << username << "' created/updated.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  LOG_INFO << "starting, " << wikicore::versionString();

  const std::string configPath =
      std::filesystem::exists("config.toml") ? "config.toml" : "config.example.toml";
  const wikicore::config::AppConfig cfg = wikicore::config::AppConfig::load(configPath);

  wikicore::index::Database db(cfg.dbPath);
  db.migrate();

  const bool createAdmin =
      argc > 1 && std::string(argv[1]) == "--create-admin";
  if (createAdmin) {
    return runCreateAdmin(db);
  }

  std::filesystem::create_directories(cfg.vaultPath);

  wikicore::auth::AdminAccount admin(db);
  wikicore::auth::SessionStore sessions(db);
  wikicore::auth::RateLimiter rateLimiter;
  wikicore::auth::AuthServices::init(sessions, rateLimiter, admin);

  if (!admin.find()) {
    LOG_WARN << "no admin account configured yet — run "
                "'wiki-server --create-admin' to set one up";
  }

  wikicore::vault::VaultRepository vault(cfg.vaultPath);
  wikicore::index::IndexUpdater indexUpdater(db);
  wikicore::vault::DocumentService documentService(vault, indexUpdater);
  wikicore::vault::AttachmentService attachmentService(vault);

  // Force these classes' DrObject<T> static registrar to actually
  // instantiate. It's a namespace-scope static (DrObject<T>::alloc_) whose
  // constructor registers the class by name in DrClassMap — but being a
  // static data member of a class *template*, the compiler only emits it
  // if something ODR-uses it. Nothing else in this program ever
  // constructs or names AuthFilter/CsrfFilter/EditPage directly (they're
  // only ever referenced by string in registerHandler's constraints /
  // newHttpViewResponse below), so without these lines the linker drops
  // them silently and every route that lists them fails at startup with
  // "middleware ... not found" (filters) or renders a blank/error page
  // (views). See drogonframework/drogon#1268 and docs/architecture.md.
  (void)wikicore::auth::AuthFilter::classTypeName();
  (void)wikicore::auth::CsrfFilter::classTypeName();
  (void)EditPage::classTypeName();

  // static/ is the ONLY thing served as static files — never the project
  // root, which would also expose config.toml/source/etc. over HTTP.
  drogon::app().setDocumentRoot("static");
  drogon::app().setClientMaxBodySize(30 * 1024 * 1024);  // headroom over the 25 MiB attachment cap

  drogon::app().registerHandler(
      "/healthz",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setBody("ok\n");
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        callback(resp);
      },
      {drogon::Get});

  wikicore::controllers::registerAuthRoutes(drogon::app());
  wikicore::controllers::registerDocumentRoutes(drogon::app(), vault, documentService,
                                                 attachmentService);

  drogon::app()
      .addListener(cfg.listenAddr, cfg.port)
      .setThreadNum(static_cast<size_t>(cfg.threads))
      .run();

  return 0;
}
