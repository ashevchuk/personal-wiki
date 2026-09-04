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
#include "auth/McpRemoteConfig.h"
#include "auth/RateLimiter.h"
#include "auth/SessionStore.h"
#include "config/AppConfig.h"
#include "controllers/AdminRoutes.h"
#include "controllers/AuthRoutes.h"
#include "controllers/DocumentRoutes.h"
#include "controllers/FolderRoutes.h"
#include "controllers/NavRoutes.h"
#include "controllers/PageRoutes.h"
#include "controllers/SearchRoutes.h"
#include "controllers/RemoteMcpRoutes.h"
#include "controllers/VersionRoutes.h"
#include "core/wikicore.h"
#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexBuilder.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "index/McpAuditLog.h"
#include "index/SnapshotStore.h"
#include "index/VaultWatcher.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/FolderService.h"
#include "vault/VaultRepository.h"

#include <drogon/drogon.h>
#include <termios.h>
#include <unistd.h>

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

// `wiki-server --reindex`: full vault rescan without starting the HTTP
// server — recovery path for a deleted/corrupted index db, or documents
// added/edited outside the app (external editor, git pull, ...).
int runReindex(const wikicore::config::AppConfig& cfg, wikicore::index::Database& db) {
  std::filesystem::create_directories(cfg.vaultPath);
  wikicore::vault::VaultRepository vault(cfg.vaultPath);
  wikicore::index::IndexUpdater indexUpdater(db);
  wikicore::index::IndexBuilder builder(vault, indexUpdater);

  const wikicore::index::RescanStats stats = builder.fullRescan();
  std::cout << "Reindexed " << stats.documentsIndexed << " document(s), removed "
            << stats.staleRowsRemoved << " stale row(s).\n";
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

  if (argc > 1 && std::string(argv[1]) == "--create-admin") {
    return runCreateAdmin(db);
  }
  if (argc > 1 && std::string(argv[1]) == "--reindex") {
    return runReindex(cfg, db);
  }

  std::filesystem::create_directories(cfg.vaultPath);

  wikicore::auth::AdminAccount admin(db);
  wikicore::auth::SessionStore sessions(db);
  wikicore::auth::RateLimiter rateLimiter;
  wikicore::auth::AuthServices::init(sessions, rateLimiter, admin);

  // Remote MCP transport settings (McpRemoteConfig.h) + its OWN,
  // separate RateLimiter instance -- deliberately not sharing state with
  // /login's limiter above. A flood of bad bearer-token attempts and a
  // flood of bad passwords are different attackers hitting different
  // surfaces; conflating them would let one drain the other's lockout
  // budget for no reason.
  wikicore::auth::McpRemoteConfig remoteMcpConfig(db);
  wikicore::auth::RateLimiter remoteMcpRateLimiter;

  if (!admin.find()) {
    LOG_WARN << "no admin account configured yet — run "
                "'wiki-server --create-admin' to set one up";
  }

  wikicore::vault::VaultRepository vault(cfg.vaultPath);
  wikicore::index::IndexUpdater indexUpdater(db);
  wikicore::index::SnapshotStore snapshotStore(db);
  wikicore::vault::DocumentService documentService(vault, indexUpdater, snapshotStore);
  // config.toml's [attachments] table REPLACES the built-in defaults
  // entirely when present (see AppConfig::attachmentMimeTypes' comment);
  // empty (the common case — nothing in config.toml) falls back to
  // AttachmentService's own defaults rather than main.cpp duplicating
  // that list a second time.
  wikicore::vault::AttachmentService attachmentService(
      vault,
      cfg.attachmentMimeTypes.empty() ? wikicore::vault::AttachmentService::defaultMimeTypes()
                                       : cfg.attachmentMimeTypes,
      cfg.attachmentInlineSafeExtensions.empty()
          ? wikicore::vault::AttachmentService::defaultInlineSafeExtensions()
          : cfg.attachmentInlineSafeExtensions);
  wikicore::index::IndexBuilder indexBuilder(vault, indexUpdater);
  wikicore::vault::FolderService folderService(vault, indexUpdater, indexBuilder);
  wikicore::index::FtsSearch ftsSearch(db);
  wikicore::index::NavQueries navQueries(db);
  // Read-only from wiki-server's side — write_access is a wiki-mcp-only
  // concept (see McpServer.cpp); this exists here purely to back the
  // admin-facing GET /api/admin/mcp-audit-log route, reading rows a
  // SEPARATE wiki-mcp process wrote through its own connection to the
  // same db file (WAL mode, same coordination VaultWatcher's own
  // separate connection already relies on).
  wikicore::index::McpAuditLog mcpAuditLog(db);

  // The db is a disposable cache, never assumed correct on faith — rescan
  // unconditionally at every startup so the index reflects whatever's
  // actually on disk (including edits made outside the app since the last
  // run). Cheap at personal-wiki scale; `--reindex` / POST /api/admin/reindex
  // exist for re-running this without a restart.
  const wikicore::index::RescanStats startupRescan = indexBuilder.fullRescan();
  LOG_INFO << "startup reindex: " << startupRescan.documentsIndexed
           << " document(s), " << startupRescan.staleRowsRemoved
           << " stale row(s) removed";

  // VaultWatcher runs on its own background thread and gets its OWN
  // sqlite3 connection to the same db file, rather than sharing `db` with
  // Drogon's request-handling threads: IndexUpdater's upsertOne/removeOne
  // each wrap a BEGIN IMMEDIATE...COMMIT, and two threads racing a
  // BEGIN on the SAME connection handle is a "cannot start a transaction
  // within a transaction" error, not a safely-serialized one — a single
  // sqlite3* handle being thread-safe (WAL mode, busy_timeout already set
  // in Database::Database) means concurrent *connections* to the same
  // file coordinate correctly; it does not mean one connection tolerates
  // concurrent callers each assuming they own its transaction state.
  wikicore::index::Database watcherDb(cfg.dbPath);
  wikicore::index::IndexUpdater watcherIndexUpdater(watcherDb);
  wikicore::index::IndexBuilder watcherIndexBuilder(vault, watcherIndexUpdater);
  wikicore::index::VaultWatcher vaultWatcher(
      cfg.vaultPath,
      [&watcherIndexBuilder, &watcherIndexUpdater](const std::string& relativePath) {
        if (!watcherIndexBuilder.reindexOneFile(relativePath)) {
          watcherIndexUpdater.removeOne(relativePath);
        }
      },
      [&watcherIndexBuilder]() {
        LOG_WARN << "inotify event queue overflowed — falling back to a full rescan";
        watcherIndexBuilder.fullRescan();
      });
  vaultWatcher.start();

  // Force these classes' DrObject<T> static registrar to actually
  // instantiate. It's a namespace-scope static (DrObject<T>::alloc_) whose
  // constructor registers the class by name in DrClassMap — but being a
  // static data member of a class *template*, the compiler only emits it
  // if something ODR-uses it. Nothing else in this program ever
  // constructs or names AuthFilter/CsrfFilter directly (they're only ever
  // referenced by string in registerHandler's constraints below), so
  // without these lines the linker drops them silently and every route
  // that lists them fails at startup with "middleware ... not found" (a
  // log line, not a build error). See drogonframework/drogon#1268 and
  // docs/architecture.md. (There used to be a matching pair of these for
  // EditPage/SearchPage CSP views — removed along with the views
  // themselves when the frontend moved to a JSON API + static SPA shell;
  // see PageRoutes.cpp.)
  (void)wikicore::auth::AuthFilter::classTypeName();
  (void)wikicore::auth::CsrfFilter::classTypeName();

  // static/ is the ONLY thing served as static files — never the project
  // root, which would also expose config.toml/source/etc. over HTTP.
  drogon::app().setDocumentRoot("static");
  drogon::app().setClientMaxBodySize(30 * 1024 * 1024);  // headroom over the 25 MiB attachment cap

  // Don't advertise the exact framework+version in every response —
  // `Server: drogon/1.9.13` by default, no reason to hand a would-be
  // attacker a free CVE-lookup hint for zero benefit to a real client.
  drogon::app().enableServerHeader(false);

  // A path that matches NEITHER a registered route NOR a real static
  // file (e.g. a typo'd URL, or the OLD pre-fix shape of a [[wiki-link]]
  // href — see WikiLinks.cpp's own commit history) used to fall through
  // to Drogon's own bare default 404 page, which is where "drogon/1.9.13"
  // was actually leaking from — not the Server header alone, the BODY of
  // that stock error page names it too. Serving the same shell.html every
  // registered page route gets, with a real 404 STATUS CODE preserved (so
  // curl/search engines/anything automated still sees a genuine 404, only
  // the HTML body differs), means router.js's own client-side fallback
  // renders instead — the exact same "nothing here" experience as any
  // other not-found case in this app, and nothing Drogon-specific ever
  // reaches the client.
  drogon::app().setDefaultHandler(
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newFileResponse("static/shell.html");
        resp->setStatusCode(drogon::k404NotFound);
        callback(resp);
      });

  // Drogon buffers large multipart request bodies to disk under its own
  // upload path (default: "./uploads" relative to CWD) BEFORE any handler
  // sees them — independent of AttachmentService's own storage. Left at
  // its default, this collides with the deployed systemd unit's hardening
  // (`ProtectSystem=strict` + `ReadWritePaths=/opt/wiki/vault_data` only):
  // caught live on first real-hardware deployment (armv7, see
  // docs/deployment.md) as a spray of "Read-only file system" errors at
  // startup, harmless for small JSON requests but fatal for actual file
  // uploads. Point it at a dot-prefixed directory INSIDE the vault (like
  // `.trash`), so it's covered by the SAME ReadWritePaths=/opt/wiki/vault_data
  // entry the systemd unit already grants, and IndexBuilder's existing
  // "skip .git/.trash/anything-dot entirely" rule (see IndexBuilder.cpp)
  // keeps these transient buffer files from ever being seen as documents
  // by fullRescan or VaultWatcher. MUST be absolute: Drogon resolves a
  // relative setUploadPath() against its document root ("static/"), not
  // the process CWD — a relative path here silently landed under
  // static/./vault_data/... instead, caught live on real-hardware
  // redeploy when the "fixed" path still hit ProtectSystem=strict.
  {
    const auto uploadPath =
        std::filesystem::absolute(std::filesystem::path(cfg.vaultPath) / ".uploads-tmp");
    std::filesystem::create_directories(uploadPath);
    drogon::app().setUploadPath(uploadPath.string());
  }

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

  wikicore::controllers::registerPageRoutes(drogon::app());
  wikicore::controllers::registerAuthRoutes(drogon::app());
  wikicore::controllers::registerDocumentRoutes(drogon::app(), vault, documentService,
                                                 attachmentService, navQueries);
  wikicore::controllers::registerSearchRoutes(drogon::app(), ftsSearch);
  wikicore::controllers::registerNavRoutes(drogon::app(), navQueries);
  wikicore::controllers::registerAdminRoutes(drogon::app(), indexBuilder, mcpAuditLog,
                                              remoteMcpConfig, cfg.vaultPath);
  wikicore::controllers::registerFolderRoutes(drogon::app(), folderService);
  wikicore::controllers::registerVersionRoutes(drogon::app(), indexUpdater, snapshotStore,
                                                documentService);
  wikicore::controllers::registerRemoteMcpRoutes(drogon::app(), remoteMcpConfig,
                                                  remoteMcpRateLimiter, ftsSearch, navQueries,
                                                  indexUpdater, documentService, mcpAuditLog);

  drogon::app()
      .addListener(cfg.listenAddr, cfg.port)
      .setThreadNum(static_cast<size_t>(cfg.threads))
      .run();

  return 0;
}
