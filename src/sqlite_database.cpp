#include "repolens/sqlite_database.hpp"
#include "repolens/interpreters/language_interpreter.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(REPOLENS_EMBEDDED_SQLITE)
#include "sqlite3.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if !defined(REPOLENS_EMBEDDED_SQLITE)
struct sqlite3;
struct sqlite3_stmt;
#endif

namespace {

constexpr int sqlite_ok = 0;
constexpr int sqlite_row = 100;
constexpr int sqlite_done = 101;
constexpr int sqlite_open_readwrite = 0x00000002;
constexpr int sqlite_open_create = 0x00000004;
constexpr int sqlite_open_fullmutex = 0x00010000;

using sqlite3_destructor_type = void (*)(void*);

sqlite3_destructor_type sqlite_transient()
{
    return reinterpret_cast<sqlite3_destructor_type>(static_cast<intptr_t>(-1));
}

#if !defined(REPOLENS_EMBEDDED_SQLITE)
template <typename T>
T load_symbol(void* library, const char* name)
{
#if defined(_WIN32)
    const auto symbol = reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    const auto symbol = reinterpret_cast<T>(dlsym(library, name));
#endif

    if (!symbol) {
        throw std::runtime_error(std::string{"SQLite library is missing symbol: "} + name);
    }

    return symbol;
}

void* open_sqlite_library()
{
#if defined(_WIN32)
    HMODULE library = LoadLibraryA("sqlite3.dll");
    if (!library) {
        library = LoadLibraryA("winsqlite3.dll");
    }
#else
    void* library = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        library = dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    }
#endif

    if (!library) {
        throw std::runtime_error(
            "SQLite runtime library was not found. Install SQLite or make sqlite3 available on PATH.");
    }

    return library;
}

void close_sqlite_library(void* library)
{
    if (!library) {
        return;
    }

#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}
#endif

struct SqliteApi {
    explicit SqliteApi()
#if defined(REPOLENS_EMBEDDED_SQLITE)
        : open_v2(sqlite3_open_v2)
        , close(sqlite3_close)
        , errmsg(sqlite3_errmsg)
        , exec(sqlite3_exec)
        , free(sqlite3_free)
        , prepare_v2(sqlite3_prepare_v2)
        , step(sqlite3_step)
        , finalize(sqlite3_finalize)
        , bind_text(sqlite3_bind_text)
        , bind_int(sqlite3_bind_int)
        , bind_int64(sqlite3_bind_int64)
        , column_text(sqlite3_column_text)
        , column_int(sqlite3_column_int)
        , column_int64(sqlite3_column_int64)
        , last_insert_rowid(sqlite3_last_insert_rowid)
#else
        : library(open_sqlite_library())
        , open_v2(load_symbol<int (*)(const char*, sqlite3**, int, const char*)>(library, "sqlite3_open_v2"))
        , close(load_symbol<int (*)(sqlite3*)>(library, "sqlite3_close"))
        , errmsg(load_symbol<const char* (*)(sqlite3*)>(library, "sqlite3_errmsg"))
        , exec(load_symbol<int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**)>(
              library,
              "sqlite3_exec"))
        , free(load_symbol<void (*)(void*)>(library, "sqlite3_free"))
        , prepare_v2(load_symbol<int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**)>(
              library,
              "sqlite3_prepare_v2"))
        , step(load_symbol<int (*)(sqlite3_stmt*)>(library, "sqlite3_step"))
        , finalize(load_symbol<int (*)(sqlite3_stmt*)>(library, "sqlite3_finalize"))
        , bind_text(load_symbol<int (*)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type)>(
              library,
              "sqlite3_bind_text"))
        , bind_int(load_symbol<int (*)(sqlite3_stmt*, int, int)>(library, "sqlite3_bind_int"))
        , bind_int64(load_symbol<int (*)(sqlite3_stmt*, int, long long)>(library, "sqlite3_bind_int64"))
        , column_text(load_symbol<const unsigned char* (*)(sqlite3_stmt*, int)>(library, "sqlite3_column_text"))
        , column_int(load_symbol<int (*)(sqlite3_stmt*, int)>(library, "sqlite3_column_int"))
        , column_int64(load_symbol<long long (*)(sqlite3_stmt*, int)>(library, "sqlite3_column_int64"))
        , last_insert_rowid(load_symbol<long long (*)(sqlite3*)>(library, "sqlite3_last_insert_rowid"))
#endif
    {
    }

    ~SqliteApi()
    {
#if !defined(REPOLENS_EMBEDDED_SQLITE)
        close_sqlite_library(library);
#endif
    }

#if !defined(REPOLENS_EMBEDDED_SQLITE)
    void* library = nullptr;
#endif
    int (*open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    const char* (*errmsg)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
    void (*free)(void*) = nullptr;
    int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
    int (*step)(sqlite3_stmt*) = nullptr;
    int (*finalize)(sqlite3_stmt*) = nullptr;
    int (*bind_text)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type) = nullptr;
    int (*bind_int)(sqlite3_stmt*, int, int) = nullptr;
    int (*bind_int64)(sqlite3_stmt*, int, long long) = nullptr;
    const unsigned char* (*column_text)(sqlite3_stmt*, int) = nullptr;
    int (*column_int)(sqlite3_stmt*, int) = nullptr;
    long long (*column_int64)(sqlite3_stmt*, int) = nullptr;
    long long (*last_insert_rowid)(sqlite3*) = nullptr;
};

std::string column_string(const SqliteApi& api, sqlite3_stmt* statement, int column)
{
    const auto* value = api.column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string{text.begin(), text.end()};
}

class Statement {
public:
    Statement(const SqliteApi& api, sqlite3* database, const std::string& sql)
        : api_(api)
    {
        if (api_.prepare_v2(database, sql.c_str(), -1, &statement_, nullptr) != sqlite_ok) {
            throw std::runtime_error(std::string{"Failed to prepare SQL: "} + api_.errmsg(database));
        }
    }

    ~Statement()
    {
        if (statement_) {
            api_.finalize(statement_);
        }
    }

    sqlite3_stmt* get() const
    {
        return statement_;
    }

    void bind_text(int index, const std::string& value)
    {
        if (api_.bind_text(statement_, index, value.c_str(), -1, sqlite_transient()) != sqlite_ok) {
            throw std::runtime_error("Failed to bind text value.");
        }
    }

    void bind_int(int index, int value)
    {
        if (api_.bind_int(statement_, index, value) != sqlite_ok) {
            throw std::runtime_error("Failed to bind integer value.");
        }
    }

    void bind_int64(int index, long long value)
    {
        if (api_.bind_int64(statement_, index, value) != sqlite_ok) {
            throw std::runtime_error("Failed to bind integer value.");
        }
    }

    void step_done(sqlite3* database)
    {
        if (api_.step(statement_) != sqlite_done) {
            throw std::runtime_error(std::string{"Failed to execute SQL statement: "} + api_.errmsg(database));
        }
    }

private:
    const SqliteApi& api_;
    sqlite3_stmt* statement_ = nullptr;
};

bool is_relation_type_candidate(const std::string& type_name)
{
    if (type_name.empty()) {
        return false;
    }

    static const std::vector<std::string> built_ins{
        "void", "bool", "byte", "sbyte", "char", "decimal", "double", "float", "int", "uint", "long", "ulong",
        "object", "short", "ushort", "string", "var"};
    for (const auto& built_in : built_ins) {
        if (type_name == built_in) {
            return false;
        }
    }

    return true;
}

} // namespace

namespace repolens {

struct SqliteDatabase::Impl {
    explicit Impl(const std::filesystem::path& database_path)
    {
        const auto flags = sqlite_open_readwrite | sqlite_open_create | sqlite_open_fullmutex;
        const auto database_path_text = path_to_utf8(database_path);
        if (api.open_v2(database_path_text.c_str(), &database, flags, nullptr) != sqlite_ok) {
            const std::string message = database ? api.errmsg(database) : "unknown SQLite error";
            throw std::runtime_error("Failed to open SQLite database: " + message);
        }
    }

    ~Impl()
    {
        if (database) {
            api.close(database);
        }
    }

    SqliteApi api;
    sqlite3* database = nullptr;
};

long long count_table_rows(const SqliteApi& api, sqlite3* database, const std::string& table_name)
{
    Statement statement{api, database, "SELECT COUNT(*) FROM " + table_name + ";"};
    const int result = api.step(statement.get());
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to count rows in "} + table_name + ": " + api.errmsg(database));
    }
    return api.column_int64(statement.get(), 0);
}

SqliteDatabase::SqliteDatabase(const std::filesystem::path& database_path)
    : impl_(new Impl(database_path))
{
}

SqliteDatabase::~SqliteDatabase()
{
    delete impl_;
}

void SqliteDatabase::exec(const std::string& sql)
{
    char* error = nullptr;
    const int result = impl_->api.exec(impl_->database, sql.c_str(), nullptr, nullptr, &error);
    if (result != sqlite_ok) {
        std::string message = error ? error : impl_->api.errmsg(impl_->database);
        if (error) {
            impl_->api.free(error);
        }
        throw std::runtime_error("SQLite error: " + message);
    }
}

void SqliteDatabase::create_schema()
{
    exec("PRAGMA foreign_keys = ON;");

    exec(
        "CREATE TABLE IF NOT EXISTS repositories ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repo_root TEXT NOT NULL,"
        "index_root TEXT NOT NULL,"
        "repo_fingerprint TEXT,"
        "schema_version INTEGER NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "last_indexed_at TEXT,"
        "read_only_repo INTEGER NOT NULL DEFAULT 1"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS files ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "relative_path TEXT NOT NULL,"
        "absolute_path TEXT NOT NULL,"
        "extension TEXT,"
        "language TEXT,"
        "size_bytes INTEGER,"
        "line_count INTEGER,"
        "char_count INTEGER,"
        "last_modified_time TEXT,"
        "content_hash TEXT,"
        "structure_hash TEXT,"
        "parse_status TEXT DEFAULT 'not_parsed',"
        "is_active INTEGER NOT NULL DEFAULT 1,"
        "first_seen_snapshot_id INTEGER,"
        "last_seen_snapshot_id INTEGER,"
        "deleted_at TEXT,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "UNIQUE(repository_id, relative_path)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS symbols ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "file_id INTEGER NOT NULL,"
        "language TEXT NOT NULL,"
        "kind TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "qualified_name TEXT,"
        "signature TEXT,"
        "return_type TEXT,"
        "visibility TEXT,"
        "modifiers TEXT,"
        "parent_symbol_id INTEGER,"
        "line_start INTEGER,"
        "line_end INTEGER,"
        "char_start INTEGER,"
        "char_end INTEGER,"
        "char_count INTEGER,"
        "content_hash TEXT,"
        "signature_hash TEXT,"
        "body_hash TEXT,"
        "stable_id TEXT,"
        "description TEXT DEFAULT '',"
        "tags TEXT DEFAULT '',"
        "ai_description TEXT DEFAULT '',"
        "ai_tags TEXT DEFAULT '',"
        "ai_model TEXT DEFAULT '',"
        "ai_enriched_at TEXT,"
        "is_active INTEGER NOT NULL DEFAULT 1,"
        "first_seen_snapshot_id INTEGER,"
        "last_seen_snapshot_id INTEGER,"
        "deleted_at TEXT,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "FOREIGN KEY(file_id) REFERENCES files(id),"
        "FOREIGN KEY(parent_symbol_id) REFERENCES symbols(id)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS symbol_parameters ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "symbol_id INTEGER NOT NULL,"
        "name TEXT NOT NULL,"
        "type TEXT,"
        "default_value TEXT,"
        "position INTEGER NOT NULL,"
        "direction TEXT,"
        "FOREIGN KEY(symbol_id) REFERENCES symbols(id)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS symbol_relations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "source_symbol_id INTEGER NOT NULL,"
        "target_symbol_id INTEGER,"
        "relation_type TEXT NOT NULL,"
        "source_text TEXT,"
        "target_text TEXT,"
        "confidence REAL DEFAULT 1.0,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "FOREIGN KEY(source_symbol_id) REFERENCES symbols(id),"
        "FOREIGN KEY(target_symbol_id) REFERENCES symbols(id)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "git_commit TEXT,"
        "description TEXT,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS changes ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "snapshot_id INTEGER NOT NULL,"
        "entity_type TEXT NOT NULL,"
        "entity_id INTEGER,"
        "change_type TEXT NOT NULL,"
        "old_hash TEXT,"
        "new_hash TEXT,"
        "old_path TEXT,"
        "new_path TEXT,"
        "created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "FOREIGN KEY(snapshot_id) REFERENCES snapshots(id)"
        ");");

    exec("CREATE INDEX IF NOT EXISTS idx_files_repo_path ON files(repository_id, relative_path);");
    exec("CREATE INDEX IF NOT EXISTS idx_files_hash ON files(content_hash);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_name ON symbols(repository_id, name);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_qualified ON symbols(repository_id, qualified_name);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_kind ON symbols(repository_id, kind);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_relations_source ON symbol_relations(source_symbol_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_relations_target ON symbol_relations(target_symbol_id);");
}

void SqliteDatabase::insert_repository(
    const std::filesystem::path& repo_root,
    const std::filesystem::path& index_root,
    int schema_version)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "INSERT INTO repositories (repo_root, index_root, schema_version, read_only_repo) VALUES (?, ?, ?, 1);"};

    const auto repo = path_to_utf8(repo_root);
    const auto index = path_to_utf8(index_root);

    statement.bind_text(1, repo);
    statement.bind_text(2, index);
    statement.bind_int(3, schema_version);
    statement.step_done(impl_->database);
}

std::optional<RepositoryStatus> SqliteDatabase::read_repository_status()
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, repo_root, index_root, schema_version, COALESCE(last_indexed_at, '') "
        "FROM repositories ORDER BY id DESC LIMIT 1;"};

    const int result = impl_->api.step(statement.get());
    if (result == sqlite_done) {
        return std::nullopt;
    }

    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read repository metadata: "} + impl_->api.errmsg(impl_->database));
    }

    RepositoryStatus status;
    status.repository_id = impl_->api.column_int64(statement.get(), 0);
    status.repo_root = column_string(impl_->api, statement.get(), 1);
    status.index_root = column_string(impl_->api, statement.get(), 2);
    status.schema_version = impl_->api.column_int(statement.get(), 3);
    status.last_indexed_at = column_string(impl_->api, statement.get(), 4);
    return status;
}

std::unordered_map<std::string, StoredFile> SqliteDatabase::read_files(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, relative_path, COALESCE(content_hash, ''), is_active FROM files WHERE repository_id = ?;"};
    statement.bind_int64(1, repository_id);

    std::unordered_map<std::string, StoredFile> files;

    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }

        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read files: "} + impl_->api.errmsg(impl_->database));
        }

        StoredFile file;
        file.id = impl_->api.column_int64(statement.get(), 0);
        file.relative_path = column_string(impl_->api, statement.get(), 1);
        file.content_hash = column_string(impl_->api, statement.get(), 2);
        file.is_active = impl_->api.column_int(statement.get(), 3) != 0;
        files.emplace(file.relative_path, file);
    }

    return files;
}

long long SqliteDatabase::create_snapshot(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "INSERT INTO snapshots (repository_id, description) VALUES (?, 'update');"};
    statement.bind_int64(1, repository_id);
    statement.step_done(impl_->database);
    return impl_->api.last_insert_rowid(impl_->database);
}

void SqliteDatabase::upsert_file(
    long long repository_id,
    const FileMetadata& file,
    long long snapshot_id,
    bool is_new_file)
{
    if (is_new_file) {
        Statement statement{
            impl_->api,
            impl_->database,
            "INSERT INTO files ("
            "repository_id, relative_path, absolute_path, extension, size_bytes, line_count, char_count, "
            "last_modified_time, content_hash, is_active, first_seen_snapshot_id, last_seen_snapshot_id"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?);"};

        statement.bind_int64(1, repository_id);
        statement.bind_text(2, file.relative_path);
        statement.bind_text(3, file.absolute_path);
        statement.bind_text(4, file.extension);
        statement.bind_int64(5, file.size_bytes);
        statement.bind_int64(6, file.line_count);
        statement.bind_int64(7, file.char_count);
        statement.bind_text(8, file.last_modified_time);
        statement.bind_text(9, file.content_hash);
        statement.bind_int64(10, snapshot_id);
        statement.bind_int64(11, snapshot_id);
        statement.step_done(impl_->database);
        return;
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE files SET "
        "absolute_path = ?, extension = ?, size_bytes = ?, line_count = ?, char_count = ?, language = ?, "
        "last_modified_time = ?, content_hash = ?, is_active = 1, last_seen_snapshot_id = ?, deleted_at = NULL "
        "WHERE repository_id = ? AND relative_path = ?;"};

    statement.bind_text(1, file.absolute_path);
    statement.bind_text(2, file.extension);
    statement.bind_int64(3, file.size_bytes);
    statement.bind_int64(4, file.line_count);
    statement.bind_int64(5, file.char_count);
    statement.bind_text(6, "");
    statement.bind_text(7, file.last_modified_time);
    statement.bind_text(8, file.content_hash);
    statement.bind_int64(9, snapshot_id);
    statement.bind_int64(10, repository_id);
    statement.bind_text(11, file.relative_path);
    statement.step_done(impl_->database);
}

void SqliteDatabase::mark_file_deleted(long long file_id, long long snapshot_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE files SET is_active = 0, last_seen_snapshot_id = ?, deleted_at = datetime('now') WHERE id = ?;"};
    statement.bind_int64(1, snapshot_id);
    statement.bind_int64(2, file_id);
    statement.step_done(impl_->database);
}

void SqliteDatabase::record_change(
    long long repository_id,
    long long snapshot_id,
    const std::string& entity_type,
    long long entity_id,
    const std::string& change_type,
    const std::string& old_hash,
    const std::string& new_hash,
    const std::string& old_path,
    const std::string& new_path)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "INSERT INTO changes ("
        "repository_id, snapshot_id, entity_type, entity_id, change_type, old_hash, new_hash, old_path, new_path"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"};

    statement.bind_int64(1, repository_id);
    statement.bind_int64(2, snapshot_id);
    statement.bind_text(3, entity_type);
    statement.bind_int64(4, entity_id);
    statement.bind_text(5, change_type);
    statement.bind_text(6, old_hash);
    statement.bind_text(7, new_hash);
    statement.bind_text(8, old_path);
    statement.bind_text(9, new_path);
    statement.step_done(impl_->database);
}

void SqliteDatabase::update_last_indexed_at(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE repositories SET last_indexed_at = datetime('now') WHERE id = ?;"};
    statement.bind_int64(1, repository_id);
    statement.step_done(impl_->database);
}

ParseSaveStats SqliteDatabase::save_parse_result(long long repository_id, long long file_id, const ParseResult& result)
{
    ParseSaveStats stats;
    {
        Statement count_statement{
            impl_->api,
            impl_->database,
            "SELECT COUNT(*) FROM symbols WHERE file_id = ?;"};
        count_statement.bind_int64(1, file_id);
        const int result_code = impl_->api.step(count_statement.get());
        if (result_code != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to count old symbols: "} + impl_->api.errmsg(impl_->database));
        }
        stats.symbols_deleted = impl_->api.column_int(count_statement.get(), 0);
    }

    {
        Statement statement{
            impl_->api,
            impl_->database,
            "DELETE FROM symbol_parameters WHERE symbol_id IN (SELECT id FROM symbols WHERE file_id = ?);"};
        statement.bind_int64(1, file_id);
        statement.step_done(impl_->database);
    }

    {
        Statement statement{
            impl_->api,
            impl_->database,
            "DELETE FROM symbol_relations WHERE source_symbol_id IN (SELECT id FROM symbols WHERE file_id = ?) "
            "OR target_symbol_id IN (SELECT id FROM symbols WHERE file_id = ?);"};
        statement.bind_int64(1, file_id);
        statement.bind_int64(2, file_id);
        statement.step_done(impl_->database);
    }

    {
        Statement statement{impl_->api, impl_->database, "DELETE FROM symbols WHERE file_id = ?;"};
        statement.bind_int64(1, file_id);
        statement.step_done(impl_->database);
    }

    std::vector<long long> inserted_symbol_ids;
    inserted_symbol_ids.reserve(result.symbols.size());

    for (const auto& symbol : result.symbols) {
        long long parent_symbol_id = 0;
        if (symbol.parent_index >= 0 && static_cast<std::size_t>(symbol.parent_index) < inserted_symbol_ids.size()) {
            parent_symbol_id = inserted_symbol_ids[static_cast<std::size_t>(symbol.parent_index)];
        }

        Statement statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbols ("
            "repository_id, file_id, language, kind, name, qualified_name, signature, return_type, visibility, modifiers, "
            "parent_symbol_id, line_start, line_end, char_start, char_end, char_count, description, tags, is_active"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, 0), ?, ?, ?, ?, ?, '', '', 1);"};

        statement.bind_int64(1, repository_id);
        statement.bind_int64(2, file_id);
        statement.bind_text(3, result.language);
        statement.bind_text(4, symbol.kind);
        statement.bind_text(5, symbol.name);
        statement.bind_text(6, symbol.qualified_name);
        statement.bind_text(7, symbol.signature);
        statement.bind_text(8, symbol.return_type);
        statement.bind_text(9, symbol.visibility);
        statement.bind_text(10, symbol.modifiers);
        statement.bind_int64(11, parent_symbol_id);
        statement.bind_int(12, symbol.line_start);
        statement.bind_int(13, symbol.line_end);
        statement.bind_int(14, symbol.char_start);
        statement.bind_int(15, symbol.char_end);
        statement.bind_int(16, symbol.char_count);
        statement.step_done(impl_->database);

        const long long symbol_id = impl_->api.last_insert_rowid(impl_->database);
        inserted_symbol_ids.push_back(symbol_id);
        ++stats.symbols_inserted;

        for (const auto& parameter : symbol.parameters) {
            Statement parameter_statement{
                impl_->api,
                impl_->database,
                "INSERT INTO symbol_parameters (symbol_id, name, type, default_value, position, direction) "
                "VALUES (?, ?, ?, ?, ?, ?);"};
            parameter_statement.bind_int64(1, symbol_id);
            parameter_statement.bind_text(2, parameter.name);
            parameter_statement.bind_text(3, parameter.type);
            parameter_statement.bind_text(4, parameter.default_value);
            parameter_statement.bind_int(5, parameter.position);
            parameter_statement.bind_text(6, parameter.direction);
            parameter_statement.step_done(impl_->database);
        }
    }

    auto insert_relation = [&](long long source_symbol_id,
                               long long target_symbol_id,
                               const std::string& relation_type,
                               const std::string& source_text,
                               const std::string& target_text) {
        Statement relation_statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbol_relations ("
            "repository_id, source_symbol_id, target_symbol_id, relation_type, source_text, target_text, confidence"
            ") VALUES (?, ?, NULLIF(?, 0), ?, ?, ?, 1.0);"};
        relation_statement.bind_int64(1, repository_id);
        relation_statement.bind_int64(2, source_symbol_id);
        relation_statement.bind_int64(3, target_symbol_id);
        relation_statement.bind_text(4, relation_type);
        relation_statement.bind_text(5, source_text);
        relation_statement.bind_text(6, target_text);
        relation_statement.step_done(impl_->database);
    };

    for (std::size_t index = 0; index < result.symbols.size(); ++index) {
        const auto& symbol = result.symbols[index];
        const long long source_symbol_id = inserted_symbol_ids[index];

        if (symbol.parent_index >= 0 && static_cast<std::size_t>(symbol.parent_index) < inserted_symbol_ids.size()) {
            const auto& parent = result.symbols[static_cast<std::size_t>(symbol.parent_index)];
            const long long parent_symbol_id = inserted_symbol_ids[static_cast<std::size_t>(symbol.parent_index)];
            insert_relation(parent_symbol_id, source_symbol_id, "contains", parent.qualified_name, symbol.qualified_name);
            insert_relation(source_symbol_id, parent_symbol_id, "contained_by", symbol.qualified_name, parent.qualified_name);
        }

        if ((symbol.kind == "method" || symbol.kind == "property") && is_relation_type_candidate(symbol.return_type)) {
            insert_relation(source_symbol_id, 0, "returns_type", symbol.qualified_name, symbol.return_type);
        }

        if (symbol.kind == "method" || symbol.kind == "constructor") {
            for (const auto& parameter : symbol.parameters) {
                if (is_relation_type_candidate(parameter.type)) {
                    insert_relation(source_symbol_id, 0, "accepts_parameter_type", symbol.qualified_name, parameter.type);
                }
            }
        }

        if (symbol.kind == "class" || symbol.kind == "interface" || symbol.kind == "struct") {
            for (std::size_t base_index = 0; base_index < symbol.base_types.size(); ++base_index) {
                const auto relation_type = symbol.kind == "class" && base_index == 0 ? "inherits" : "implements";
                insert_relation(source_symbol_id, 0, relation_type, symbol.qualified_name, symbol.base_types[base_index]);
            }
        }
    }

    Statement file_statement{
        impl_->api,
        impl_->database,
        "UPDATE files SET language = ?, parse_status = ? WHERE id = ?;"};
    file_statement.bind_text(1, result.language);
    file_statement.bind_text(2, result.success ? "parsed" : "failed");
    file_statement.bind_int64(3, file_id);
    file_statement.step_done(impl_->database);
    return stats;
}

int SqliteDatabase::mark_symbols_inactive_for_file(long long file_id)
{
    int deactivated = 0;
    {
        Statement count_statement{
            impl_->api,
            impl_->database,
            "SELECT COUNT(*) FROM symbols WHERE file_id = ? AND is_active = 1;"};
        count_statement.bind_int64(1, file_id);
        const int result = impl_->api.step(count_statement.get());
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to count active file symbols: "} + impl_->api.errmsg(impl_->database));
        }
        deactivated = impl_->api.column_int(count_statement.get(), 0);
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE symbols SET is_active = 0, deleted_at = datetime('now') WHERE file_id = ? AND is_active = 1;"};
    statement.bind_int64(1, file_id);
    statement.step_done(impl_->database);
    return deactivated;
}

int SqliteDatabase::count_active_symbols(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT COUNT(*) FROM symbols WHERE repository_id = ? AND is_active = 1;"};
    statement.bind_int64(1, repository_id);

    const int result = impl_->api.step(statement.get());
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to count symbols: "} + impl_->api.errmsg(impl_->database));
    }

    return impl_->api.column_int(statement.get(), 0);
}

DatabaseRowCounts SqliteDatabase::count_rows()
{
    DatabaseRowCounts counts;
    counts.repositories = count_table_rows(impl_->api, impl_->database, "repositories");
    counts.files = count_table_rows(impl_->api, impl_->database, "files");
    counts.symbols = count_table_rows(impl_->api, impl_->database, "symbols");
    counts.symbol_parameters = count_table_rows(impl_->api, impl_->database, "symbol_parameters");
    counts.symbol_relations = count_table_rows(impl_->api, impl_->database, "symbol_relations");
    counts.snapshots = count_table_rows(impl_->api, impl_->database, "snapshots");
    counts.changes = count_table_rows(impl_->api, impl_->database, "changes");
    return counts;
}

std::vector<SearchResult> SqliteDatabase::search(long long repository_id, const SearchOptions& options)
{
    std::vector<SearchResult> results;
    const int limit = options.limit <= 0 ? 20 : options.limit;
    const std::string pattern = "%" + options.query + "%";

    if (options.kind.empty() || options.kind != "file") {
        const bool has_kind_filter = !options.kind.empty();
        Statement statement{
            impl_->api,
            impl_->database,
            has_kind_filter
                ? "SELECT s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
                  "f.relative_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
                  "FROM symbols s JOIN files f ON f.id = s.file_id "
                  "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 AND s.kind = ? "
                  "AND (s.name LIKE ? OR COALESCE(s.qualified_name, '') LIKE ? OR COALESCE(s.signature, '') LIKE ?) "
                  "ORDER BY s.kind, s.name, f.relative_path LIMIT ?;"
                : "SELECT s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
                  "f.relative_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
                  "FROM symbols s JOIN files f ON f.id = s.file_id "
                  "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
                  "AND (s.name LIKE ? OR COALESCE(s.qualified_name, '') LIKE ? OR COALESCE(s.signature, '') LIKE ?) "
                  "ORDER BY s.kind, s.name, f.relative_path LIMIT ?;"};

        int bind_index = 1;
        statement.bind_int64(bind_index++, repository_id);
        if (has_kind_filter) {
            statement.bind_text(bind_index++, options.kind);
        }
        statement.bind_text(bind_index++, pattern);
        statement.bind_text(bind_index++, pattern);
        statement.bind_text(bind_index++, pattern);
        statement.bind_int(bind_index++, limit);

        while (results.size() < static_cast<std::size_t>(limit)) {
            const int result = impl_->api.step(statement.get());
            if (result == sqlite_done) {
                break;
            }
            if (result != sqlite_row) {
                throw std::runtime_error(std::string{"Failed to search symbols: "} + impl_->api.errmsg(impl_->database));
            }

            SearchResult item;
            item.result_type = "symbol";
            item.kind = column_string(impl_->api, statement.get(), 0);
            item.name = column_string(impl_->api, statement.get(), 1);
            item.qualified_name = column_string(impl_->api, statement.get(), 2);
            item.signature = column_string(impl_->api, statement.get(), 3);
            item.file_path = column_string(impl_->api, statement.get(), 4);
            item.line_start = impl_->api.column_int(statement.get(), 5);
            item.line_end = impl_->api.column_int(statement.get(), 6);
            results.push_back(item);
        }
    }

    if ((options.kind.empty() || options.kind == "file") && results.size() < static_cast<std::size_t>(limit)) {
        Statement statement{
            impl_->api,
            impl_->database,
            "SELECT relative_path FROM files "
            "WHERE repository_id = ? AND is_active = 1 AND (relative_path LIKE ? OR absolute_path LIKE ?) "
            "ORDER BY relative_path LIMIT ?;"};
        statement.bind_int64(1, repository_id);
        statement.bind_text(2, pattern);
        statement.bind_text(3, pattern);
        statement.bind_int(4, limit - static_cast<int>(results.size()));

        while (results.size() < static_cast<std::size_t>(limit)) {
            const int result = impl_->api.step(statement.get());
            if (result == sqlite_done) {
                break;
            }
            if (result != sqlite_row) {
                throw std::runtime_error(std::string{"Failed to search files: "} + impl_->api.errmsg(impl_->database));
            }

            SearchResult item;
            item.result_type = "file";
            item.kind = "file";
            item.name = column_string(impl_->api, statement.get(), 0);
            item.file_path = item.name;
            results.push_back(item);
        }
    }

    return results;
}

std::vector<ContextSymbolCandidate> SqliteDatabase::find_context_symbols(
    long long repository_id,
    const std::string& symbol_name,
    bool partial_match)
{
    const std::string pattern = "%" + symbol_name + "%";
    Statement statement{
        impl_->api,
        impl_->database,
        partial_match
            ? "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
              "f.relative_path, f.absolute_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "AND (s.name = ? OR COALESCE(s.qualified_name, '') = ? OR s.name LIKE ? "
              "OR COALESCE(s.qualified_name, '') LIKE ? OR COALESCE(s.signature, '') LIKE ?) "
              "ORDER BY CASE WHEN s.name = ? OR COALESCE(s.qualified_name, '') = ? THEN 0 ELSE 1 END, "
              "s.kind, s.qualified_name, f.relative_path;"
            : "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
              "f.relative_path, f.absolute_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "AND (s.name = ? OR COALESCE(s.qualified_name, '') = ?) "
              "ORDER BY s.kind, s.qualified_name, f.relative_path;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, symbol_name);
    statement.bind_text(3, symbol_name);
    if (partial_match) {
        statement.bind_text(4, pattern);
        statement.bind_text(5, pattern);
        statement.bind_text(6, pattern);
        statement.bind_text(7, symbol_name);
        statement.bind_text(8, symbol_name);
    }

    std::vector<ContextSymbolCandidate> results;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to find context symbols: "} + impl_->api.errmsg(impl_->database));
        }

        ContextSymbolCandidate item;
        item.symbol_id = impl_->api.column_int64(statement.get(), 0);
        item.kind = column_string(impl_->api, statement.get(), 1);
        item.name = column_string(impl_->api, statement.get(), 2);
        item.qualified_name = column_string(impl_->api, statement.get(), 3);
        item.signature = column_string(impl_->api, statement.get(), 4);
        item.relative_path = column_string(impl_->api, statement.get(), 5);
        item.absolute_path = column_string(impl_->api, statement.get(), 6);
        item.line_start = impl_->api.column_int(statement.get(), 7);
        item.line_end = impl_->api.column_int(statement.get(), 8);
        results.push_back(item);
    }

    return results;
}

std::vector<ContextSymbolCandidate> SqliteDatabase::active_context_symbols(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
        "f.relative_path, f.absolute_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
        "FROM symbols s JOIN files f ON f.id = s.file_id "
        "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
        "ORDER BY length(s.name) DESC, s.qualified_name, f.relative_path;"};
    statement.bind_int64(1, repository_id);

    std::vector<ContextSymbolCandidate> results;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read active context symbols: "} + impl_->api.errmsg(impl_->database));
        }

        ContextSymbolCandidate item;
        item.symbol_id = impl_->api.column_int64(statement.get(), 0);
        item.kind = column_string(impl_->api, statement.get(), 1);
        item.name = column_string(impl_->api, statement.get(), 2);
        item.qualified_name = column_string(impl_->api, statement.get(), 3);
        item.signature = column_string(impl_->api, statement.get(), 4);
        item.relative_path = column_string(impl_->api, statement.get(), 5);
        item.absolute_path = column_string(impl_->api, statement.get(), 6);
        item.line_start = impl_->api.column_int(statement.get(), 7);
        item.line_end = impl_->api.column_int(statement.get(), 8);
        results.push_back(item);
    }

    return results;
}

std::vector<ContextRelatedSymbol> SqliteDatabase::find_related_type_symbols(
    long long repository_id,
    const std::vector<long long>& source_symbol_ids)
{
    std::vector<ContextRelatedSymbol> related;

    for (const long long source_symbol_id : source_symbol_ids) {
        Statement relation_statement{
            impl_->api,
            impl_->database,
            "SELECT r.relation_type, COALESCE(r.target_text, ''), COALESCE(s.qualified_name, '') "
            "FROM symbol_relations r JOIN symbols s ON s.id = r.source_symbol_id "
            "WHERE r.repository_id = ? AND r.source_symbol_id = ? AND s.is_active = 1 "
            "AND r.relation_type IN ('returns_type', 'accepts_parameter_type', 'inherits', 'implements') "
            "ORDER BY r.relation_type, r.target_text;"};
        relation_statement.bind_int64(1, repository_id);
        relation_statement.bind_int64(2, source_symbol_id);

        while (true) {
            const int relation_result = impl_->api.step(relation_statement.get());
            if (relation_result == sqlite_done) {
                break;
            }
            if (relation_result != sqlite_row) {
                throw std::runtime_error(std::string{"Failed to read symbol relations: "} + impl_->api.errmsg(impl_->database));
            }

            const auto relation_type = column_string(impl_->api, relation_statement.get(), 0);
            const auto target_text = column_string(impl_->api, relation_statement.get(), 1);
            const auto source_qualified_name = column_string(impl_->api, relation_statement.get(), 2);

            Statement symbol_statement{
                impl_->api,
                impl_->database,
                "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
                "f.relative_path, f.absolute_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
                "FROM symbols s JOIN files f ON f.id = s.file_id "
                "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
                "AND s.kind IN ('class', 'interface', 'struct', 'enum') "
                "AND (s.name = ? OR COALESCE(s.qualified_name, '') = ?) "
                "ORDER BY s.qualified_name LIMIT 5;"};
            symbol_statement.bind_int64(1, repository_id);
            symbol_statement.bind_text(2, target_text);
            symbol_statement.bind_text(3, target_text);

            while (true) {
                const int symbol_result = impl_->api.step(symbol_statement.get());
                if (symbol_result == sqlite_done) {
                    break;
                }
                if (symbol_result != sqlite_row) {
                    throw std::runtime_error(std::string{"Failed to read related type symbols: "} + impl_->api.errmsg(impl_->database));
                }

                ContextRelatedSymbol item;
                item.relation_type = relation_type;
                item.source_qualified_name = source_qualified_name;
                item.symbol.symbol_id = impl_->api.column_int64(symbol_statement.get(), 0);
                item.symbol.kind = column_string(impl_->api, symbol_statement.get(), 1);
                item.symbol.name = column_string(impl_->api, symbol_statement.get(), 2);
                item.symbol.qualified_name = column_string(impl_->api, symbol_statement.get(), 3);
                item.symbol.signature = column_string(impl_->api, symbol_statement.get(), 4);
                item.symbol.relative_path = column_string(impl_->api, symbol_statement.get(), 5);
                item.symbol.absolute_path = column_string(impl_->api, symbol_statement.get(), 6);
                item.symbol.line_start = impl_->api.column_int(symbol_statement.get(), 7);
                item.symbol.line_end = impl_->api.column_int(symbol_statement.get(), 8);
                related.push_back(item);
            }
        }
    }

    return related;
}

std::vector<std::string> SqliteDatabase::active_file_paths(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT relative_path FROM files WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
    statement.bind_int64(1, repository_id);

    std::vector<std::string> paths;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read active file paths: "} + impl_->api.errmsg(impl_->database));
        }

        paths.push_back(column_string(impl_->api, statement.get(), 0));
    }

    return paths;
}

std::vector<SymbolForEnrichment> SqliteDatabase::symbols_for_enrichment(long long repository_id, bool changed_only)
{
    Statement statement{
        impl_->api,
        impl_->database,
        changed_only
            ? "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
              "f.relative_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "AND (COALESCE(s.ai_enriched_at, '') = '' OR COALESCE(s.ai_description, '') = '') "
              "ORDER BY s.qualified_name;"
            : "SELECT s.id, s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(s.signature, ''), "
              "f.relative_path, COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "ORDER BY s.qualified_name;"};
    statement.bind_int64(1, repository_id);

    std::vector<SymbolForEnrichment> symbols;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read symbols for enrichment: "} + impl_->api.errmsg(impl_->database));
        }

        SymbolForEnrichment symbol;
        symbol.symbol_id = impl_->api.column_int64(statement.get(), 0);
        symbol.kind = column_string(impl_->api, statement.get(), 1);
        symbol.name = column_string(impl_->api, statement.get(), 2);
        symbol.qualified_name = column_string(impl_->api, statement.get(), 3);
        symbol.signature = column_string(impl_->api, statement.get(), 4);
        symbol.file_path = column_string(impl_->api, statement.get(), 5);
        symbol.line_start = impl_->api.column_int(statement.get(), 6);
        symbol.line_end = impl_->api.column_int(statement.get(), 7);
        symbols.push_back(symbol);
    }

    return symbols;
}

void SqliteDatabase::update_symbol_enrichment(long long symbol_id, const EnrichmentResult& result)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE symbols SET "
        "description = ?, tags = ?, ai_description = ?, ai_tags = ?, ai_model = ?, ai_enriched_at = datetime('now') "
        "WHERE id = ? AND is_active = 1;"};
    statement.bind_text(1, result.description);
    statement.bind_text(2, result.tags);
    statement.bind_text(3, result.ai_description);
    statement.bind_text(4, result.ai_tags);
    statement.bind_text(5, result.ai_model);
    statement.bind_int64(6, symbol_id);
    statement.step_done(impl_->database);
}

} // namespace repolens
