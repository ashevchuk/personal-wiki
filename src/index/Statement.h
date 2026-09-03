#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace wikicore::index {

// Thin RAII wrapper over sqlite3_stmt: prepares on construction, always
// finalizes, and turns "forgot to check the return code" into an
// exception instead of a silently-ignored failure. Introduced once
// IndexUpdater needed enough distinct queries that hand-rolled
// prepare/bind/step/finalize (as in AdminAccount/SessionStore) started
// getting genuinely repetitive and error-prone.
class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("prepare failed: ") +
                                sqlite3_errmsg(db_));
    }
  }

  ~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement& bind(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    return *this;
  }

  Statement& bind(int index, int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
    return *this;
  }

  Statement& bindNull(int index) {
    sqlite3_bind_null(stmt_, index);
    return *this;
  }

  // For INSERT/UPDATE/DELETE — throws unless sqlite reports SQLITE_DONE.
  void run() {
    if (sqlite3_step(stmt_) != SQLITE_DONE) {
      throw std::runtime_error(std::string("statement failed: ") +
                                sqlite3_errmsg(db_));
    }
  }

  // For SELECT — true while a row is available.
  bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }

  std::string columnText(int index) const {
    const auto* text =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    return text ? std::string(text) : std::string();
  }

  int64_t columnInt64(int index) const {
    return sqlite3_column_int64(stmt_, index);
  }

  int64_t lastInsertRowId() const { return sqlite3_last_insert_rowid(db_); }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace wikicore::index
