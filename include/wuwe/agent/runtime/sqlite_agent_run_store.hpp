#ifndef WUWE_AGENT_RUNTIME_SQLITE_AGENT_RUN_STORE_HPP
#define WUWE_AGENT_RUNTIME_SQLITE_AGENT_RUN_STORE_HPP

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <wuwe/agent/runtime/run_store.hpp>
#include <wuwe/agent/core/filesystem.hpp>
#include <wuwe/agent/core/sqlite_schema.hpp>

#if WUWE_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace wuwe::agent::runtime {

class sqlite_agent_run_store final : public agent_run_store {
public:
#if WUWE_HAS_SQLITE
  static constexpr int latest_schema_version = 1;

  explicit sqlite_agent_run_store(std::filesystem::path path)
      : path_(std::move(path)) {
    try {
      open();
      initialize_schema();
    }
    catch (...) {
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      throw;
    }
  }

  ~sqlite_agent_run_store() override {
    if (db_) {
      sqlite3_close(db_);
    }
  }

  sqlite_agent_run_store(const sqlite_agent_run_store&) = delete;
  sqlite_agent_run_store& operator=(const sqlite_agent_run_store&) = delete;

  [[nodiscard]] agent_run_store_capabilities capabilities()
    const noexcept override {
    return {
      .declared = true,
      .durable = true,
      .transactional = true,
      .optimistic_concurrency = true,
      .atomic_mutations = true,
      .ordered_replay = true,
      .schema_migrations = true,
      .multi_process_safe = true,
      .coordination_scope = run_store_coordination_scope::single_node,
      .schema_version = latest_schema_version,
    };
  }

  [[nodiscard]] int schema_version() const {
    std::scoped_lock lock(mutex_);
    return current_schema_version().value_or(0);
  }

  run_store_write_result create(
    agent_run_record record,
    agent_run_event event) override {
    if (record.id.empty()) {
      throw std::invalid_argument("agent run store requires a run id");
    }
    if (event.type.empty()) {
      throw std::invalid_argument("agent run store requires an event type");
    }
    std::scoped_lock lock(mutex_);
    transaction tx(*this);
    const auto existing_revision = current_revision(record.id);
    if (existing_revision) {
      tx.rollback();
      return {
        .status = run_store_write_status::already_exists,
        .revision = *existing_revision,
      };
    }

    record.revision = 1;
    event.run_id = record.id;
    event.sequence = record.revision;
    event.status = record.status;
    const auto document = agent_run_record_to_json(record).dump();
    statement insert_run(db_,
      "INSERT INTO agent_runs (id, revision, document_json) VALUES (?, ?, ?)");
    bind_text(insert_run.get(), 1, record.id);
    sqlite3_bind_int64(insert_run.get(), 2,
      static_cast<sqlite3_int64>(record.revision));
    bind_text(insert_run.get(), 3, document);
    step_done(insert_run.get(), "create agent run");
    insert_event(event);
    tx.commit();
    return { .revision = record.revision };
  }

  std::optional<agent_run_record> load(
    const std::string& run_id) const override {
    std::scoped_lock lock(mutex_);
    statement select(db_,
      "SELECT document_json FROM agent_runs WHERE id = ?");
    bind_text(select.get(), 1, run_id);
    const auto rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE) {
      return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
      throw_sqlite("load agent run");
    }
    return parse_record(column_text(select.get(), 0));
  }

  run_store_write_result update(
    std::uint64_t expected_revision,
    agent_run_record record,
    agent_run_event event) override {
    if (record.id.empty()) {
      throw std::invalid_argument("agent run store requires a run id");
    }
    if (event.type.empty()) {
      throw std::invalid_argument("agent run store requires an event type");
    }
    std::scoped_lock lock(mutex_);
    transaction tx(*this);
    const auto actual_revision = current_revision(record.id);
    if (!actual_revision) {
      tx.rollback();
      return { .status = run_store_write_status::not_found };
    }
    if (*actual_revision != expected_revision) {
      tx.rollback();
      return {
        .status = run_store_write_status::conflict,
        .revision = *actual_revision,
      };
    }

    record.revision = expected_revision + 1;
    event.run_id = record.id;
    event.sequence = record.revision;
    event.status = record.status;
    statement update_run(db_,
      "UPDATE agent_runs SET revision = ?, document_json = ? "
      "WHERE id = ? AND revision = ?");
    sqlite3_bind_int64(update_run.get(), 1,
      static_cast<sqlite3_int64>(record.revision));
    bind_text(update_run.get(), 2, agent_run_record_to_json(record).dump());
    bind_text(update_run.get(), 3, record.id);
    sqlite3_bind_int64(update_run.get(), 4,
      static_cast<sqlite3_int64>(expected_revision));
    step_done(update_run.get(), "update agent run");
    if (sqlite3_changes(db_) != 1) {
      tx.rollback();
      return {
        .status = run_store_write_status::conflict,
        .revision = current_revision(record.id).value_or(0),
      };
    }
    insert_event(event);
    tx.commit();
    return { .revision = record.revision };
  }

  std::vector<agent_run_event> list_events(
    const std::string& run_id,
    std::uint64_t after_sequence = 0) const override {
    std::scoped_lock lock(mutex_);
    statement select(db_,
      "SELECT document_json FROM agent_run_events "
      "WHERE run_id = ? AND sequence > ? ORDER BY sequence ASC");
    bind_text(select.get(), 1, run_id);
    sqlite3_bind_int64(select.get(), 2,
      static_cast<sqlite3_int64>(after_sequence));
    std::vector<agent_run_event> output;
    while (true) {
      const auto rc = sqlite3_step(select.get());
      if (rc == SQLITE_DONE) {
        break;
      }
      if (rc != SQLITE_ROW) {
        throw_sqlite("list agent run events");
      }
      const auto parsed = nlohmann::json::parse(column_text(select.get(), 0));
      output.push_back(agent_run_event_from_json(parsed));
    }
    return output;
  }

private:
  class statement {
  public:
    statement(sqlite3* db, const char* sql) : db_(db) {
      if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
        throw std::runtime_error(
          "prepare sqlite agent run statement failed: " +
          std::string(sqlite3_errmsg(db_)));
      }
    }

    ~statement() {
      if (statement_) {
        sqlite3_finalize(statement_);
      }
    }

    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
      return statement_;
    }

  private:
    sqlite3* db_ {};
    sqlite3_stmt* statement_ {};
  };

  class transaction {
  public:
    explicit transaction(sqlite_agent_run_store& store) : store_(store) {
      store_.exec("BEGIN IMMEDIATE");
    }

    ~transaction() {
      if (active_) {
        try {
          store_.exec("ROLLBACK");
        }
        catch (...) {
        }
      }
    }

    void commit() {
      store_.exec("COMMIT");
      active_ = false;
    }

    void rollback() {
      store_.exec("ROLLBACK");
      active_ = false;
    }

  private:
    sqlite_agent_run_store& store_;
    bool active_ { true };
  };

  void open() {
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }
    const auto path_utf8 = core::filesystem_path_to_utf8(path_);
    if (sqlite3_open(path_utf8.c_str(), &db_) != SQLITE_OK) {
      const auto message = db_ ? sqlite3_errmsg(db_) : "unknown sqlite error";
      const std::string owned_message(message);
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      throw std::runtime_error(
        "failed to open sqlite agent run store: " + owned_message);
    }
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA busy_timeout = 5000");
    exec("PRAGMA journal_mode = WAL");
  }

  void initialize_schema() {
    transaction tx(*this);
    exec(
      "CREATE TABLE IF NOT EXISTS agent_runtime_schema ("
      "component TEXT PRIMARY KEY, version INTEGER NOT NULL)");
    core::validate_sqlite_table(db_, "agent_runtime_schema", {
      { "component", "TEXT", false, 1 },
      { "version", "INTEGER", true, 0 },
    }, "sqlite agent run store");
    auto version = current_schema_version().value_or(0);
    if (version > latest_schema_version) {
      throw std::runtime_error(
        "sqlite agent run store schema version " + std::to_string(version) +
        " is newer than supported version " +
        std::to_string(latest_schema_version));
    }
    while (version < latest_schema_version) {
      switch (version) {
        case 0:
          migrate_0_to_1();
          version = 1;
          break;
        default:
          throw std::runtime_error(
            "no sqlite agent run store migration from version " +
            std::to_string(version));
      }
      set_schema_version(version);
    }
    validate_schema();
    tx.commit();
  }

  void migrate_0_to_1() {
    exec(
      "CREATE TABLE IF NOT EXISTS agent_runs ("
      "id TEXT PRIMARY KEY,"
      "revision INTEGER NOT NULL,"
      "document_json TEXT NOT NULL)");
    exec(
      "CREATE TABLE IF NOT EXISTS agent_run_events ("
      "run_id TEXT NOT NULL,"
      "sequence INTEGER NOT NULL,"
      "document_json TEXT NOT NULL,"
      "PRIMARY KEY (run_id, sequence),"
      "FOREIGN KEY (run_id) REFERENCES agent_runs(id) ON DELETE CASCADE)");
  }

  void validate_schema() const {
    core::validate_sqlite_table(db_, "agent_runtime_schema", {
      { "component", "TEXT", false, 1 },
      { "version", "INTEGER", true, 0 },
    }, "sqlite agent run store");
    core::validate_sqlite_table(db_, "agent_runs", {
      { "id", "TEXT", false, 1 },
      { "revision", "INTEGER", true, 0 },
      { "document_json", "TEXT", true, 0 },
    }, "sqlite agent run store");
    core::validate_sqlite_table(db_, "agent_run_events", {
      { "run_id", "TEXT", true, 1 },
      { "sequence", "INTEGER", true, 2 },
      { "document_json", "TEXT", true, 0 },
    }, "sqlite agent run store");
  }

  [[nodiscard]] std::optional<int> current_schema_version() const {
    statement version(db_,
      "SELECT version FROM agent_runtime_schema WHERE component = 'run_store'");
    const auto rc = sqlite3_step(version.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) throw_sqlite("read agent run schema version");
    return sqlite3_column_int(version.get(), 0);
  }

  void set_schema_version(int version) {
    statement update(db_,
      "INSERT INTO agent_runtime_schema (component, version) VALUES ('run_store', ?) "
      "ON CONFLICT(component) DO UPDATE SET version = excluded.version");
    sqlite3_bind_int(update.get(), 1, version);
    step_done(update.get(), "update agent run schema version");
  }

  void insert_event(const agent_run_event& event) {
    statement insert(db_,
      "INSERT INTO agent_run_events (run_id, sequence, document_json) "
      "VALUES (?, ?, ?)");
    bind_text(insert.get(), 1, event.run_id);
    sqlite3_bind_int64(insert.get(), 2,
      static_cast<sqlite3_int64>(event.sequence));
    bind_text(insert.get(), 3, agent_run_event_to_json(event).dump());
    step_done(insert.get(), "insert agent run event");
  }

  [[nodiscard]] std::optional<std::uint64_t> current_revision(
    const std::string& run_id) const {
    statement select(db_, "SELECT revision FROM agent_runs WHERE id = ?");
    bind_text(select.get(), 1, run_id);
    const auto rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE) {
      return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
      throw_sqlite("read agent run revision");
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 0));
  }

  [[nodiscard]] static agent_run_record parse_record(const std::string& text) {
    return agent_run_record_from_json(nlohmann::json::parse(text));
  }

  void exec(const char* sql) const {
    char* error {};
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      const std::string message = error ? error : "unknown sqlite error";
      sqlite3_free(error);
      throw std::runtime_error("sqlite agent run store failed: " + message);
    }
  }

  void throw_sqlite(const char* operation) const {
    throw std::runtime_error(
      std::string(operation) + " failed: " + sqlite3_errmsg(db_));
  }

  void step_done(sqlite3_stmt* statement, const char* operation) const {
    if (sqlite3_step(statement) != SQLITE_DONE) {
      throw_sqlite(operation);
    }
  }

  static void bind_text(sqlite3_stmt* statement, int index,
    const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(),
      static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }

  static std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string {};
  }

  std::filesystem::path path_;
  sqlite3* db_ {};
  mutable std::mutex mutex_;
#else
  explicit sqlite_agent_run_store(std::filesystem::path) {
    throw std::runtime_error(
      "sqlite_agent_run_store requires WUWE_HAS_SQLITE=1");
  }

  [[nodiscard]] agent_run_store_capabilities capabilities()
    const noexcept override {
    return {};
  }

  run_store_write_result create(
    agent_run_record,
    agent_run_event) override {
    return { .status = run_store_write_status::not_found };
  }

  std::optional<agent_run_record> load(const std::string&) const override {
    return std::nullopt;
  }

  run_store_write_result update(
    std::uint64_t,
    agent_run_record,
    agent_run_event) override {
    return { .status = run_store_write_status::not_found };
  }

  std::vector<agent_run_event> list_events(
    const std::string&,
    std::uint64_t = 0) const override {
    return {};
  }
#endif
};

} // namespace wuwe::agent::runtime

#endif // WUWE_AGENT_RUNTIME_SQLITE_AGENT_RUN_STORE_HPP
