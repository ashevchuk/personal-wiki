#include "index/Database.h"

#include "index/schema.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wikicore::index {

namespace {

void execOrThrow(sqlite3* db, const char* sql) {
  char* errMsg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::string msg = errMsg ? errMsg : "unknown sqlite error";
    sqlite3_free(errMsg);
    throw std::runtime_error("sqlite error: " + msg);
  }
}

const char* kSessionPragmas[] = {
    "PRAGMA journal_mode = WAL;",
    "PRAGMA foreign_keys = ON;",
    "PRAGMA busy_timeout = 5000;",
};

// Ordered list of migrations; index 0 is schema_version 1, etc. Add new
// entries at the end only — never edit or reorder an already-shipped one.
constexpr std::array<const char*, 6> kMigrations = {schema::kMigration1, schema::kMigration2,
                                                      schema::kMigration3, schema::kMigration4,
                                                      schema::kMigration5, schema::kMigration6};

#ifdef WIKI_ENABLE_SQLITE_VEC
// Deliberately NOT `#include <sqlite-vec.h>` here — that header pulls in
// sqlite3ext.h unless SQLITE_CORE is defined, which redefines every
// sqlite3_* symbol as a macro through a sqlite3_api diversion table this
// file never initializes (it's not itself an extension entry point) —
// that would silently break execOrThrow/sqlite3_open_v2/etc. everywhere
// else in this file. sqlite-vec.c (the loadable-extension side, where
// that diversion table IS the correct and intended pattern — see its own
// sqlite3_vec_init()) is compiled as its own separate translation unit
// (root CMakeLists.txt), so this is the ONLY declaration this file
// needs: the exact signature sqlite-vec.h itself declares, reproduced by
// hand instead of including the header that comes with side effects
// this file doesn't want.
struct sqlite3_api_routines;
extern "C" int sqlite3_vec_init(sqlite3* db, char** pzErrMsg,
                                 const sqlite3_api_routines* pApi);

// sqlite3_auto_extension() registers vec0 for every sqlite3_open() the
// process makes from here on (this Database's own connection AND
// VaultWatcher's separate one — see docs/architecture.md's "VaultWatcher
// gets its OWN Database" entry) — process-wide and permanent, so it only
// needs doing once regardless of how many Database instances get
// constructed. std::call_once (same shape as
// LocalEmbeddingProvider::ensureBackendInit()) guards that.
std::once_flag g_sqliteVecInitFlag;

void ensureSqliteVecRegistered() {
  std::call_once(g_sqliteVecInitFlag, [] {
    sqlite3_auto_extension(reinterpret_cast<void (*)()>(sqlite3_vec_init));
  });
}
#endif

std::string stampUtc() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

void renameIfExists(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  if (!std::filesystem::exists(from, ec)) return;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(from, ec);
  }
}

std::optional<std::string> textCol(sqlite3_stmt* stmt, int i) {
  if (sqlite3_column_type(stmt, i) == SQLITE_NULL) return std::nullopt;
  const auto* t = sqlite3_column_text(stmt, i);
  return t ? std::optional<std::string>{reinterpret_cast<const char*>(t)} : std::string{};
}

struct SalvagedUser {
  std::string username;
  std::string passwordHash;
  std::string createdAt;
};

struct SalvagedSession {
  std::string tokenHash;
  std::string createdAt;
  std::string expiresAt;
  std::string lastSeenAt;
  std::string csrfToken;
  std::optional<std::string> userAgent;
  std::optional<std::string> ip;
};

struct SalvagedMcp {
  int enabled = 0;
  int writeEnabled = 0;
  std::optional<std::string> tokenHash;
};

struct SalvagedAudit {
  std::string at;
  std::string toolName;
  std::string path;
  int success = 0;
  std::optional<std::string> detail;
};

struct SalvagedChat {
  std::string id;
  std::string title;
  std::string createdAt;
  std::string updatedAt;
  std::string eventsJson;
  std::string messagesJson;
};

struct Salvage {
  std::optional<SalvagedUser> user;
  std::vector<SalvagedSession> sessions;
  std::optional<SalvagedMcp> mcp;
  std::vector<std::string> cidrs;
  std::optional<int> vectorSearchEnabled;
  std::vector<SalvagedAudit> audits;
  std::vector<SalvagedChat> chats;
};

// Every SELECT here is best-effort: a torn FTS/vec0 page must not prevent
// us from copying the admin row that still sits on an earlier, intact
// page. Swallow prepare/step failures per table, never abort the salvage.
Salvage salvageAuth(sqlite3* db) {
  Salvage out;
  if (!db) return out;

  auto run = [db](const char* sql, const auto& onRow) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      try {
        onRow(stmt);
      } catch (...) {
        break;
      }
    }
    sqlite3_finalize(stmt);
  };

  run("SELECT username, password_hash, created_at FROM users WHERE id = 1",
      [&](sqlite3_stmt* stmt) {
        const auto u = textCol(stmt, 0);
        const auto h = textCol(stmt, 1);
        const auto c = textCol(stmt, 2);
        if (u && h && c) out.user = SalvagedUser{*u, *h, *c};
      });

  run("SELECT token_hash, created_at, expires_at, last_seen_at, csrf_token, "
      "user_agent, ip FROM sessions",
      [&](sqlite3_stmt* stmt) {
        const auto th = textCol(stmt, 0);
        const auto c = textCol(stmt, 1);
        const auto e = textCol(stmt, 2);
        const auto l = textCol(stmt, 3);
        const auto csrf = textCol(stmt, 4);
        if (!th || !c || !e || !l || !csrf) return;
        SalvagedSession s;
        s.tokenHash = *th;
        s.createdAt = *c;
        s.expiresAt = *e;
        s.lastSeenAt = *l;
        s.csrfToken = *csrf;
        s.userAgent = textCol(stmt, 5);
        s.ip = textCol(stmt, 6);
        out.sessions.push_back(std::move(s));
      });

  run("SELECT enabled, write_enabled, token_hash FROM mcp_remote_config WHERE id = 1",
      [&](sqlite3_stmt* stmt) {
        SalvagedMcp m;
        m.enabled = sqlite3_column_int(stmt, 0);
        m.writeEnabled = sqlite3_column_int(stmt, 1);
        m.tokenHash = textCol(stmt, 2);
        out.mcp = m;
      });

  run("SELECT cidr FROM mcp_remote_allowed_cidrs", [&](sqlite3_stmt* stmt) {
    if (const auto c = textCol(stmt, 0)) out.cidrs.push_back(*c);
  });

  run("SELECT vector_search_enabled FROM embeddings_runtime_config WHERE id = 1",
      [&](sqlite3_stmt* stmt) { out.vectorSearchEnabled = sqlite3_column_int(stmt, 0); });

  run("SELECT at, tool_name, path, success, detail FROM mcp_audit_log ORDER BY id",
      [&](sqlite3_stmt* stmt) {
        const auto at = textCol(stmt, 0);
        const auto tool = textCol(stmt, 1);
        const auto path = textCol(stmt, 2);
        if (!at || !tool || !path) return;
        SalvagedAudit a;
        a.at = *at;
        a.toolName = *tool;
        a.path = *path;
        a.success = sqlite3_column_int(stmt, 3);
        a.detail = textCol(stmt, 4);
        out.audits.push_back(std::move(a));
      });

  run("SELECT id, title, created_at, updated_at, events_json, messages_json FROM agent_chats",
      [&](sqlite3_stmt* stmt) {
        const auto id = textCol(stmt, 0);
        const auto title = textCol(stmt, 1);
        const auto created = textCol(stmt, 2);
        const auto updated = textCol(stmt, 3);
        const auto events = textCol(stmt, 4);
        const auto messages = textCol(stmt, 5);
        if (!id || !title || !created || !updated || !events || !messages) return;
        SalvagedChat c;
        c.id = *id;
        c.title = *title;
        c.createdAt = *created;
        c.updatedAt = *updated;
        c.eventsJson = *events;
        c.messagesJson = *messages;
        out.chats.push_back(std::move(c));
      });

  return out;
}

void bindText(sqlite3_stmt* stmt, int i, const std::string& value) {
  sqlite3_bind_text(stmt, i, value.c_str(), -1, SQLITE_TRANSIENT);
}

void bindTextOpt(sqlite3_stmt* stmt, int i, const std::optional<std::string>& value) {
  if (!value)
    sqlite3_bind_null(stmt, i);
  else
    bindText(stmt, i, *value);
}

sqlite3_stmt* mustPrepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(db));
  }
  return stmt;
}

bool stepDone(sqlite3_stmt* stmt) {
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

void restoreAuth(sqlite3* db, const Salvage& s) {
  if (!db) return;
  execOrThrow(db, "BEGIN IMMEDIATE;");
  try {
    if (s.user) {
      sqlite3_stmt* stmt = mustPrepare(db,
                         "INSERT INTO users(id, username, password_hash, created_at) "
                         "VALUES (1, ?1, ?2, ?3);");
      bindText(stmt, 1, s.user->username);
      bindText(stmt, 2, s.user->passwordHash);
      bindText(stmt, 3, s.user->createdAt);
      if (!stepDone(stmt)) throw std::runtime_error("restore users failed");
    }
    if (s.user) {
      for (const auto& sess : s.sessions) {
        sqlite3_stmt* stmt = mustPrepare(db,
                           "INSERT INTO sessions(token_hash, user_id, created_at, expires_at, "
                           "last_seen_at, csrf_token, user_agent, ip) "
                           "VALUES (?1, 1, ?2, ?3, ?4, ?5, ?6, ?7);");
        bindText(stmt, 1, sess.tokenHash);
        bindText(stmt, 2, sess.createdAt);
        bindText(stmt, 3, sess.expiresAt);
        bindText(stmt, 4, sess.lastSeenAt);
        bindText(stmt, 5, sess.csrfToken);
        bindTextOpt(stmt, 6, sess.userAgent);
        bindTextOpt(stmt, 7, sess.ip);
        if (!stepDone(stmt)) throw std::runtime_error("restore sessions failed");
      }
    }
    if (s.mcp) {
      sqlite3_stmt* stmt = mustPrepare(db,
                         "INSERT INTO mcp_remote_config(id, enabled, write_enabled, token_hash) "
                         "VALUES (1, ?1, ?2, ?3);");
      sqlite3_bind_int(stmt, 1, s.mcp->enabled);
      sqlite3_bind_int(stmt, 2, s.mcp->writeEnabled);
      bindTextOpt(stmt, 3, s.mcp->tokenHash);
      if (!stepDone(stmt)) throw std::runtime_error("restore mcp_remote_config failed");
    }
    for (const auto& cidr : s.cidrs) {
      sqlite3_stmt* stmt = mustPrepare(db, "INSERT OR IGNORE INTO mcp_remote_allowed_cidrs(cidr) VALUES (?1);");
      bindText(stmt, 1, cidr);
      if (!stepDone(stmt)) throw std::runtime_error("restore cidr failed");
    }
    if (s.vectorSearchEnabled.has_value()) {
      sqlite3_stmt* stmt = mustPrepare(
          db,
          "INSERT INTO embeddings_runtime_config(id, vector_search_enabled) VALUES (1, ?1) "
          "ON CONFLICT(id) DO UPDATE SET vector_search_enabled = excluded.vector_search_enabled;");
      sqlite3_bind_int(stmt, 1, *s.vectorSearchEnabled);
      if (!stepDone(stmt)) throw std::runtime_error("restore embeddings_runtime_config failed");
    }
    for (const auto& a : s.audits) {
      sqlite3_stmt* stmt = mustPrepare(db,
                         "INSERT INTO mcp_audit_log(at, tool_name, path, success, detail) "
                         "VALUES (?1, ?2, ?3, ?4, ?5);");
      bindText(stmt, 1, a.at);
      bindText(stmt, 2, a.toolName);
      bindText(stmt, 3, a.path);
      sqlite3_bind_int(stmt, 4, a.success);
      bindTextOpt(stmt, 5, a.detail);
      if (!stepDone(stmt)) throw std::runtime_error("restore mcp_audit_log failed");
    }
    for (const auto& c : s.chats) {
      sqlite3_stmt* stmt = mustPrepare(
          db,
          "INSERT INTO agent_chats(id, title, created_at, updated_at, events_json, messages_json) "
          "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
      bindText(stmt, 1, c.id);
      bindText(stmt, 2, c.title);
      bindText(stmt, 3, c.createdAt);
      bindText(stmt, 4, c.updatedAt);
      bindText(stmt, 5, c.eventsJson);
      bindText(stmt, 6, c.messagesJson);
      if (!stepDone(stmt)) throw std::runtime_error("restore agent_chats failed");
    }
    execOrThrow(db, "COMMIT;");
  } catch (...) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

}  // namespace

Database::Database(std::filesystem::path dbPath) : path_(std::move(dbPath)) {
#ifdef WIKI_ENABLE_SQLITE_VEC
  ensureSqliteVecRegistered();
#endif

  try {
    openConnection();
  } catch (const std::exception&) {
    // Unreadable file (not even a SQLite header) — same power-loss class
    // as a torn page, but we cannot SELECT anything out of it. Quarantine
    // and start empty rather than refusing to boot. ensureUsable() reports
    // this so wiki-server can log "run --create-admin".
    closeConnection();
    try {
      constructorQuarantined_ = quarantineFiles();
      pruneOldQuarantines();
    } catch (...) {
    }
    openConnection();
  }
}

Database::~Database() { closeConnection(); }

void Database::openConnection() {
  closeConnection();
  if (path_.has_parent_path()) {
    std::filesystem::create_directories(path_.parent_path());
  }

  const int rc =
      sqlite3_open_v2(path_.string().c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr);
  if (rc != SQLITE_OK) {
    const std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
    closeConnection();
    throw std::runtime_error("failed to open database '" + path_.string() + "': " + msg);
  }

  // WAL + foreign_keys are session pragmas (not persisted in the schema),
  // so they're set on every open, not just at migration time.
  for (const char* sql : kSessionPragmas) {
    execOrThrow(db_, sql);
  }
}

void Database::closeConnection() {
  if (!db_) return;
  sqlite3_close(db_);
  db_ = nullptr;
}

int Database::currentSchemaVersion() const {
  // index_meta doesn't exist yet on a brand-new db — that's version 0.
  sqlite3_stmt* checkTable = nullptr;
  sqlite3_prepare_v2(
      db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='index_meta'", -1, &checkTable,
      nullptr);
  const bool tableExists = sqlite3_step(checkTable) == SQLITE_ROW;
  sqlite3_finalize(checkTable);
  if (!tableExists) return 0;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "SELECT value FROM index_meta WHERE key = 'schema_version'", -1, &stmt,
                     nullptr);
  int version = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    version = std::atoi(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return version;
}

void Database::migrate() {
  int version = currentSchemaVersion();
  const int target = static_cast<int>(kMigrations.size());

  for (int v = version; v < target; ++v) {
    execOrThrow(db_, "BEGIN IMMEDIATE;");
    try {
      execOrThrow(db_, kMigrations[static_cast<size_t>(v)]);

      const std::string upsert =
          "INSERT INTO index_meta(key, value) VALUES ('schema_version', '" + std::to_string(v + 1) +
          "') ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
      execOrThrow(db_, upsert.c_str());

      execOrThrow(db_, "COMMIT;");
    } catch (...) {
      execOrThrow(db_, "ROLLBACK;");
      throw;
    }
  }
}

std::vector<std::string> Database::integrityErrors() const {
  std::vector<std::string> errors;
  if (!db_) {
    errors.emplace_back("no connection");
    return errors;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA integrity_check;", -1, &stmt, nullptr) != SQLITE_OK) {
    errors.emplace_back(sqlite3_errmsg(db_));
    return errors;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* v = sqlite3_column_text(stmt, 0);
    if (!v) continue;
    const std::string row(reinterpret_cast<const char*>(v));
    if (row != "ok") errors.push_back(row);
  }
  sqlite3_finalize(stmt);
  return errors;
}

RecoverResult Database::ensureUsable() {
  if (!constructorQuarantined_.empty()) {
    RecoverResult result;
    result.rebuilt = true;
    result.quarantined = constructorQuarantined_;
    result.notes.emplace_back("file was not a readable SQLite database");
    try {
      migrate();
    } catch (const std::exception& e) {
      result.notes.push_back(std::string("schema migrate failed: ") + e.what());
      throw;
    }
    return result;
  }
  try {
    migrate();
  } catch (const std::exception& e) {
    return rebuildPreservingAuth(std::string("schema migrate failed: ") + e.what());
  }
  const auto errors = integrityErrors();
  if (errors.empty()) return {};
  std::string why = "integrity_check failed";
  if (!errors.front().empty()) why += ": " + errors.front();
  if (errors.size() > 1) why += " (+" + std::to_string(errors.size() - 1) + " more)";
  return rebuildPreservingAuth(why);
}

RecoverResult Database::rebuildPreservingAuth(const std::string& reason) {
  RecoverResult result;
  result.rebuilt = true;
  result.notes.push_back(reason);

  Salvage salvaged;
  try {
    salvaged = salvageAuth(db_);
  } catch (...) {
    salvaged = {};
  }
  result.adminRestored = salvaged.user.has_value();
  result.mcpRestored = salvaged.mcp.has_value();

  closeConnection();
  result.quarantined = quarantineFiles();
  pruneOldQuarantines();
  openConnection();
  migrate();
  try {
    restoreAuth(db_, salvaged);
  } catch (const std::exception& e) {
    result.adminRestored = false;
    result.mcpRestored = false;
    result.notes.push_back(std::string("could not restore salvaged rows: ") + e.what());
  }
  const auto after = integrityErrors();
  if (!after.empty()) {
    throw std::runtime_error("fresh index db still fails integrity_check: " + after.front());
  }
  return result;
}

std::filesystem::path Database::quarantineFiles() {
  const std::string suffix = ".corrupt-" + stampUtc();
  auto destFor = [&](const std::filesystem::path& src) {
    return std::filesystem::path(src.string() + suffix);
  };
  const auto dest = destFor(path_);
  renameIfExists(path_, dest);
  renameIfExists(std::filesystem::path(path_.string() + "-wal"), destFor(path_.string() + "-wal"));
  renameIfExists(std::filesystem::path(path_.string() + "-shm"), destFor(path_.string() + "-shm"));
  return dest;
}

void Database::pruneOldQuarantines() {
  // Keep the three most recent quarantines so a dying SD card cannot fill
  // the disk with one corrupt copy per reboot. Timestamp is in the
  // filename (sortable).
  const std::string prefix = path_.filename().string() + ".corrupt-";
  std::vector<std::filesystem::path> found;
  std::error_code ec;
  const auto dir = path_.parent_path().empty() ? std::filesystem::current_path() : path_.parent_path();
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) found.push_back(entry.path());
  }
  if (found.size() <= 3) return;
  std::sort(found.begin(), found.end());
  const std::size_t drop = found.size() - 3;
  for (std::size_t i = 0; i < drop; ++i) {
    const auto& p = found[i];
    std::filesystem::remove(p, ec);
    std::filesystem::remove(std::filesystem::path(p.string() + "-wal"), ec);
    // Sidecars were named path-wal.corrupt-TS, not dest-wal.
    const auto stem = p.filename().string();  // index.db.corrupt-TS
    // Also remove matching wal/shm quarantines with the same TS suffix.
    const auto tsPos = stem.rfind(".corrupt-");
    if (tsPos == std::string::npos) continue;
    const std::string ts = stem.substr(tsPos);  // .corrupt-TS
    std::filesystem::remove(dir / (path_.filename().string() + "-wal" + ts), ec);
    std::filesystem::remove(dir / (path_.filename().string() + "-shm" + ts), ec);
  }
}

}  // namespace wikicore::index
