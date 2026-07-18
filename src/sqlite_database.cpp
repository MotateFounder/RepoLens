#include "repolens/sqlite_database.hpp"
#include "repolens/interpreters/language_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
        , reset(sqlite3_reset)
        , clear_bindings(sqlite3_clear_bindings)
        , finalize(sqlite3_finalize)
        , bind_text(sqlite3_bind_text)
        , bind_int(sqlite3_bind_int)
        , bind_int64(sqlite3_bind_int64)
        , bind_double(sqlite3_bind_double)
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
        , reset(load_symbol<int (*)(sqlite3_stmt*)>(library, "sqlite3_reset"))
        , clear_bindings(load_symbol<int (*)(sqlite3_stmt*)>(library, "sqlite3_clear_bindings"))
        , finalize(load_symbol<int (*)(sqlite3_stmt*)>(library, "sqlite3_finalize"))
        , bind_text(load_symbol<int (*)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type)>(
              library,
              "sqlite3_bind_text"))
        , bind_int(load_symbol<int (*)(sqlite3_stmt*, int, int)>(library, "sqlite3_bind_int"))
        , bind_int64(load_symbol<int (*)(sqlite3_stmt*, int, long long)>(library, "sqlite3_bind_int64"))
        , bind_double(load_symbol<int (*)(sqlite3_stmt*, int, double)>(library, "sqlite3_bind_double"))
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
    int (*reset)(sqlite3_stmt*) = nullptr;
    int (*clear_bindings)(sqlite3_stmt*) = nullptr;
    int (*finalize)(sqlite3_stmt*) = nullptr;
    int (*bind_text)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type) = nullptr;
    int (*bind_int)(sqlite3_stmt*, int, int) = nullptr;
    int (*bind_int64)(sqlite3_stmt*, int, long long) = nullptr;
    int (*bind_double)(sqlite3_stmt*, int, double) = nullptr;
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

std::string ascii_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
std::string normalize_fact_path(std::string path)
{
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }

    return std::filesystem::path{path}.lexically_normal().generic_string();
}

std::string make_stable_symbol_id(
    const std::string& file_path,
    const std::string& language,
    const std::string& kind,
    const std::string& qualified_name,
    const std::string& name,
    const std::string& signature,
    int line_start)
{
    std::ostringstream output;
    output
        << normalize_fact_path(file_path) << '|'
        << language << '|'
        << kind << '|'
        << (qualified_name.empty() ? name : qualified_name) << '|'
        << signature << '|'
        << line_start;
    return output.str();
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

    void bind_double(int index, double value)
    {
        if (api_.bind_double(statement_, index, value) != sqlite_ok) {
            throw std::runtime_error("Failed to bind double value.");
        }
    }

    void step_done(sqlite3* database)
    {
        if (api_.step(statement_) != sqlite_done) {
            throw std::runtime_error(std::string{"Failed to execute SQL statement: "} + api_.errmsg(database));
        }
    }

    void reset(sqlite3* database)
    {
        if (api_.reset(statement_) != sqlite_ok) {
            throw std::runtime_error(std::string{"Failed to reset SQL statement: "} + api_.errmsg(database));
        }
        if (api_.clear_bindings(statement_) != sqlite_ok) {
            throw std::runtime_error("Failed to clear SQL bindings.");
        }
    }

private:
    const SqliteApi& api_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    Transaction(const SqliteApi& api, sqlite3* database)
        : api_(api)
        , database_(database)
    {
        exec("BEGIN IMMEDIATE TRANSACTION;");
    }

    ~Transaction()
    {
        if (!committed_) {
            try {
                exec("ROLLBACK;");
            } catch (...) {
            }
        }
    }

    void commit()
    {
        exec("COMMIT;");
        committed_ = true;
    }

private:
    void exec(const char* sql)
    {
        char* error = nullptr;
        const int result = api_.exec(database_, sql, nullptr, nullptr, &error);
        if (result != sqlite_ok) {
            std::string message = error ? error : api_.errmsg(database_);
            if (error) {
                api_.free(error);
            }
            throw std::runtime_error("SQLite transaction error: " + message);
        }
    }

    const SqliteApi& api_;
    sqlite3* database_ = nullptr;
    bool committed_ = false;
};

std::vector<std::string> read_source_lines(const std::string& path)
{
    std::vector<std::string> lines;
    std::ifstream file{path};
    if (!file) {
        return lines;
    }
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool is_identifier_char(char value)
{
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_' || value == '$';
}

bool has_identifier_boundaries(const std::string& text, std::size_t position, std::size_t length)
{
    const bool left_ok = position == 0 || !is_identifier_char(text[position - 1]);
    const auto right = position + length;
    const bool right_ok = right >= text.size() || !is_identifier_char(text[right]);
    return left_ok && right_ok;
}

bool is_unresolved_call_noise(const std::string& name)
{
    static const std::vector<std::string> keywords{
        "if", "for", "while", "switch", "catch", "return", "sizeof", "decltype", "static_cast", "dynamic_cast",
        "reinterpret_cast", "const_cast", "new", "delete", "using", "namespace", "class", "struct", "enum", "try"};
    return std::find(keywords.begin(), keywords.end(), name) != keywords.end();
}

std::string trim_copy(std::string value);

struct ResolverSymbol {
    long long id = 0;
    long long parent_id = 0;
    std::size_t local_index = static_cast<std::size_t>(-1);
    bool local = false;
    std::string file_path;
    std::string language;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string parent_qualified_name;
    int line_start = 0;
    int line_end = 0;
};

struct ReferenceToken {
    std::string text;
    std::string name;
    std::string qualifier;
    std::string receiver;
    int line = 0;
    int column = 0;
    bool call = false;
};

struct ResolutionOutcome {
    long long target_symbol_id = 0;
    std::string target_text;
    std::string strategy = "unresolved_call_text";
    std::string evidence;
    double confidence = 0.25;
    bool unresolved = true;
};

std::string symbol_display_name(const ResolverSymbol& symbol)
{
    return symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
}

std::string qualifier_of(const std::string& qualified_name)
{
    const auto cpp = qualified_name.rfind("::");
    const auto dot = qualified_name.rfind('.');
    std::size_t position = std::string::npos;
    if (cpp != std::string::npos && dot != std::string::npos) {
        position = std::max(cpp, dot);
    } else if (cpp != std::string::npos) {
        position = cpp;
    } else {
        position = dot;
    }
    return position == std::string::npos ? std::string{} : qualified_name.substr(0, position);
}

std::string root_namespace_of(const std::string& qualified_name)
{
    const auto cpp = qualified_name.find("::");
    const auto dot = qualified_name.find('.');
    std::size_t position = std::string::npos;
    if (cpp != std::string::npos && dot != std::string::npos) {
        position = std::min(cpp, dot);
    } else if (cpp != std::string::npos) {
        position = cpp;
    } else {
        position = dot;
    }
    return position == std::string::npos ? std::string{} : qualified_name.substr(0, position);
}

bool qualified_suffix_matches(const std::string& qualified_name, const std::string& query)
{
    if (query.empty() || qualified_name.size() <= query.size()) {
        return qualified_name == query;
    }
    if (qualified_name.compare(qualified_name.size() - query.size(), query.size(), query) != 0) {
        return false;
    }
    const auto separator_index = qualified_name.size() - query.size();
    return separator_index >= 2 && qualified_name.substr(separator_index - 2, 2) == "::" ||
           separator_index >= 1 && qualified_name[separator_index - 1] == '.';
}

bool imported_context_matches(const std::vector<std::string>& import_lines, const ResolverSymbol& candidate)
{
    const auto qualified = symbol_display_name(candidate);
    const auto scope = qualifier_of(qualified);
    if (scope.empty()) {
        return false;
    }
    for (const auto& import_line : import_lines) {
        if (import_line.find(scope) != std::string::npos || import_line.find(root_namespace_of(qualified)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> line_identifiers(const std::string& line)
{
    std::vector<std::string> identifiers;
    for (std::size_t position = 0; position < line.size(); ++position) {
        if (!std::isalpha(static_cast<unsigned char>(line[position])) && line[position] != '_') {
            continue;
        }
        const auto start = position;
        ++position;
        while (position < line.size() && is_identifier_char(line[position])) {
            ++position;
        }
        identifiers.push_back(line.substr(start, position - start));
    }
    return identifiers;
}

struct ReceiverTypeEvidence {
    std::string type;
    std::string source;
    std::string evidence;
    double confidence = 0.0;
};

std::string strip_type_noise(std::string value)
{
    value = trim_copy(value);
    while (!value.empty() && (value.back() == ';' || value.back() == ',' || value.back() == ')' || value.back() == '{')) {
        value.pop_back();
        value = trim_copy(value);
    }
    const std::vector<std::string> noise{"const", "volatile", "mutable", "static", "class", "struct", "enum", "typename"};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& word : noise) {
            if (value == word) {
                return {};
            }
            if (value.rfind(word + " ", 0) == 0) {
                value = trim_copy(value.substr(word.size() + 1));
                changed = true;
            }
        }
    }
    while (!value.empty() && (value.back() == '*' || value.back() == '&')) {
        value.pop_back();
        value = trim_copy(value);
    }
    return value;
}

std::string innermost_template_argument(const std::string& value)
{
    const auto open = value.find('<');
    const auto close = value.rfind('>');
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return {};
    }
    std::string inner = value.substr(open + 1, close - open - 1);
    int depth = 0;
    for (std::size_t index = 0; index < inner.size(); ++index) {
        if (inner[index] == '<') {
            ++depth;
        } else if (inner[index] == '>') {
            --depth;
        } else if (inner[index] == ',' && depth == 0) {
            inner = inner.substr(0, index);
            break;
        }
    }
    return trim_copy(inner);
}

std::string normalize_type_name(std::string value, const std::unordered_map<std::string, std::string>& aliases)
{
    value = strip_type_noise(value);
    if (value.empty()) {
        return {};
    }
    const auto templated = innermost_template_argument(value);
    if (!templated.empty()) {
        const auto base = value.substr(0, value.find('<'));
        if (base.find("unique_ptr") != std::string::npos || base.find("shared_ptr") != std::string::npos ||
            base.find("weak_ptr") != std::string::npos || base.find("optional") != std::string::npos) {
            value = templated;
        }
    }
    value = strip_type_noise(value);
    const auto alias = aliases.find(value);
    if (alias != aliases.end()) {
        return normalize_type_name(alias->second, aliases);
    }
    const auto suffix = value.find_last_of(':');
    if (suffix != std::string::npos && suffix + 1 < value.size()) {
        const auto short_alias = aliases.find(value.substr(suffix + 1));
        if (short_alias != aliases.end()) {
            return normalize_type_name(short_alias->second, aliases);
        }
    }
    return value;
}

std::unordered_map<std::string, std::string> collect_type_aliases(const std::vector<std::string>& source_lines)
{
    std::unordered_map<std::string, std::string> aliases;
    for (const auto& raw_line : source_lines) {
        const auto line = trim_copy(raw_line);
        if (line.rfind("using ", 0) == 0) {
            const auto equals = line.find('=');
            if (equals != std::string::npos) {
                auto alias = trim_copy(line.substr(6, equals - 6));
                auto target = trim_copy(line.substr(equals + 1));
                if (!alias.empty()) {
                    aliases[alias] = strip_type_noise(target);
                }
            }
        } else if (line.rfind("typedef ", 0) == 0) {
            auto body = strip_type_noise(line.substr(8));
            const auto identifiers = line_identifiers(body);
            if (identifiers.size() >= 2) {
                aliases[identifiers.back()] = body.substr(0, body.rfind(identifiers.back()));
            }
        }
    }
    return aliases;
}

std::optional<std::string> local_declaration_type_for_receiver(
    const std::string& line,
    const std::string& receiver,
    const std::unordered_map<std::string, std::string>& aliases)
{
    const auto receiver_position = line.find(receiver);
    if (receiver_position == std::string::npos || !has_identifier_boundaries(line, receiver_position, receiver.size())) {
        return std::nullopt;
    }
    const auto after = receiver_position + receiver.size();
    std::size_t cursor = after;
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
        ++cursor;
    }
    if (cursor < line.size() && (line[cursor] == '(' || line[cursor] == '.')) {
        return std::nullopt;
    }
    auto before = trim_copy(line.substr(0, receiver_position));
    if (before.empty() || before.find('=') != std::string::npos || before.find("return") != std::string::npos) {
        return std::nullopt;
    }
    const auto paren = before.rfind('(');
    if (paren != std::string::npos) {
        before = trim_copy(before.substr(paren + 1));
    }
    const auto comma = before.rfind(',');
    if (comma != std::string::npos) {
        before = trim_copy(before.substr(comma + 1));
    }
    const auto normalized = normalize_type_name(before, aliases);
    if (normalized.empty() || is_unresolved_call_noise(normalized)) {
        return std::nullopt;
    }
    return normalized;
}

ReceiverTypeEvidence infer_receiver_type(
    const repolens::ParseResult& result,
    std::size_t source_index,
    const std::vector<std::string>& source_lines,
    const std::unordered_map<std::string, std::string>& aliases,
    const std::string& receiver,
    int line_number)
{
    ReceiverTypeEvidence evidence;
    if (receiver.empty() || source_index >= result.symbols.size()) {
        return evidence;
    }
    const auto& source = result.symbols[source_index];
    for (const auto& parameter : source.parameters) {
        if (parameter.name == receiver) {
            evidence.type = normalize_type_name(parameter.type, aliases);
            evidence.source = "parameter";
            evidence.evidence = "receiver '" + receiver + "' matched parameter type '" + parameter.type + "'";
            evidence.confidence = 0.97;
            return evidence;
        }
    }
    if (source.parent_index >= 0 && static_cast<std::size_t>(source.parent_index) < result.symbols.size()) {
        for (const auto& symbol : result.symbols) {
            if (symbol.kind == "field" && symbol.name == receiver && symbol.parent_index == source.parent_index) {
                evidence.type = normalize_type_name(symbol.return_type, aliases);
                evidence.source = "field";
                evidence.evidence = "receiver '" + receiver + "' matched class field type '" + symbol.return_type + "'";
                evidence.confidence = 0.94;
                return evidence;
            }
        }
    }
    const int start = std::max(1, source.line_start);
    const int end = std::min(line_number, static_cast<int>(source_lines.size()));
    for (int line = end; line >= start; --line) {
        const auto inferred = local_declaration_type_for_receiver(source_lines[static_cast<std::size_t>(line - 1)], receiver, aliases);
        if (inferred && !inferred->empty()) {
            evidence.type = *inferred;
            evidence.source = "local_variable";
            evidence.evidence = "receiver '" + receiver + "' matched local declaration type '" + *inferred + "'";
            evidence.confidence = aliases.find(*inferred) == aliases.end() ? 0.96 : 0.92;
            return evidence;
        }
    }
    return evidence;
}

std::vector<ReferenceToken> extract_call_tokens(const std::string& line, int line_number)
{
    std::vector<ReferenceToken> tokens;
    for (std::size_t position = 0; position < line.size(); ++position) {
        if (!std::isalpha(static_cast<unsigned char>(line[position])) && line[position] != '_') {
            continue;
        }
        const auto start = position;
        ++position;
        while (position < line.size() && is_identifier_char(line[position])) {
            ++position;
        }
        std::size_t cursor = position;
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        if (cursor >= line.size() || line[cursor] != '(') {
            continue;
        }
        ReferenceToken token;
        token.name = line.substr(start, position - start);
        if (is_unresolved_call_noise(token.name)) {
            continue;
        }
        token.text = token.name;
        token.line = line_number;
        token.column = static_cast<int>(start + 1);
        token.call = true;

        std::size_t left = start;
        while (left > 0 && std::isspace(static_cast<unsigned char>(line[left - 1]))) {
            --left;
        }
        if (left >= 2 && line.substr(left - 2, 2) == "::") {
            std::size_t qualifier_end = left - 2;
            std::size_t qualifier_start = qualifier_end;
            while (qualifier_start > 0) {
                const char previous = line[qualifier_start - 1];
                if (is_identifier_char(previous) || previous == ':' || previous == '.') {
                    --qualifier_start;
                } else {
                    break;
                }
            }
            token.qualifier = line.substr(qualifier_start, qualifier_end - qualifier_start);
            token.text = token.qualifier + "::" + token.name;
        } else if (left >= 1 && line[left - 1] == '.') {
            std::size_t receiver_end = left - 1;
            std::size_t receiver_start = receiver_end;
            while (receiver_start > 0 && is_identifier_char(line[receiver_start - 1])) {
                --receiver_start;
            }
            token.receiver = line.substr(receiver_start, receiver_end - receiver_start);
            token.text = token.receiver + "." + token.name;
        } else if (left >= 2 && line.substr(left - 2, 2) == "->") {
            std::size_t receiver_end = left - 2;
            std::size_t receiver_start = receiver_end;
            while (receiver_start > 0 && is_identifier_char(line[receiver_start - 1])) {
                --receiver_start;
            }
            token.receiver = line.substr(receiver_start, receiver_end - receiver_start);
            token.text = token.receiver + "->" + token.name;
        }
        tokens.push_back(token);
    }
    return tokens;
}

ResolutionOutcome choose_unique_candidate(
    const std::vector<const ResolverSymbol*>& candidates,
    const std::string& strategy,
    const std::string& evidence,
    double confidence)
{
    ResolutionOutcome outcome;
    if (candidates.size() == 1) {
        outcome.target_symbol_id = candidates.front()->id;
        outcome.target_text = symbol_display_name(*candidates.front());
        outcome.strategy = strategy;
        outcome.evidence = evidence;
        outcome.confidence = confidence;
        outcome.unresolved = false;
        return outcome;
    }
    outcome.strategy = candidates.empty() ? "unresolved_call_text" : "unresolved_ambiguous";
    outcome.evidence = candidates.empty() ? "no candidate matched resolver cascade" : "multiple candidates matched " + strategy;
    outcome.confidence = candidates.empty() ? 0.25 : 0.45;
    return outcome;
}

ResolutionOutcome resolve_reference_token(
    const ReferenceToken& token,
    const std::vector<ResolverSymbol>& candidates,
    const ResolverSymbol& source_symbol,
    const std::vector<std::string>& import_lines,
    const ReceiverTypeEvidence& receiver_type)
{
    auto collect = [&](const auto& predicate) {
        std::vector<const ResolverSymbol*> matches;
        for (const auto& candidate : candidates) {
            if (candidate.id == source_symbol.id || candidate.name != token.name) {
                continue;
            }
            if (predicate(candidate)) {
                matches.push_back(&candidate);
            }
        }
        std::sort(matches.begin(), matches.end(), [](const auto* left, const auto* right) {
            if (left->file_path != right->file_path) {
                return left->file_path < right->file_path;
            }
            if (left->line_start != right->line_start) {
                return left->line_start < right->line_start;
            }
            return symbol_display_name(*left) < symbol_display_name(*right);
        });
        return matches;
    };

    if (!token.qualifier.empty()) {
        auto matches = collect([&](const ResolverSymbol& candidate) {
            return symbol_display_name(candidate) == token.text;
        });
        auto outcome = choose_unique_candidate(matches, "exact_fully_qualified", "token text matched qualified symbol", 1.0);
        if (!outcome.unresolved || !matches.empty()) {
            return outcome;
        }
    }

    if (!receiver_type.type.empty()) {
        auto matches = collect([&](const ResolverSymbol& candidate) {
            const auto parent = candidate.parent_qualified_name.empty() ? qualifier_of(symbol_display_name(candidate)) : candidate.parent_qualified_name;
            return parent == receiver_type.type || qualified_suffix_matches(parent, receiver_type.type);
        });
        auto outcome = choose_unique_candidate(matches, "receiver_type", receiver_type.evidence, receiver_type.confidence);
        if (!outcome.unresolved || !matches.empty()) {
            return outcome;
        }
    }

    const auto source_scope = qualifier_of(symbol_display_name(source_symbol));
    if (!source_scope.empty()) {
        auto matches = collect([&](const ResolverSymbol& candidate) {
            return candidate.file_path == source_symbol.file_path && qualifier_of(symbol_display_name(candidate)) == source_scope;
        });
        auto outcome = choose_unique_candidate(matches, "same_scope_match", "candidate shares source scope '" + source_scope + "'", 0.93);
        if (!outcome.unresolved || !matches.empty()) {
            return outcome;
        }
    }

    auto same_file = collect([&](const ResolverSymbol& candidate) {
        return candidate.file_path == source_symbol.file_path;
    });
    auto same_file_outcome = choose_unique_candidate(same_file, "same_file_match", "unique same-file candidate", 0.9);
    if (!same_file_outcome.unresolved || !same_file.empty()) {
        return same_file_outcome;
    }

    const auto source_root = root_namespace_of(symbol_display_name(source_symbol));
    if (!source_root.empty()) {
        auto matches = collect([&](const ResolverSymbol& candidate) {
            return root_namespace_of(symbol_display_name(candidate)) == source_root;
        });
        auto outcome = choose_unique_candidate(matches, "same_namespace_match", "candidate shares namespace/module root '" + source_root + "'", 0.86);
        if (!outcome.unresolved || !matches.empty()) {
            return outcome;
        }
    }

    auto imported = collect([&](const ResolverSymbol& candidate) {
        return imported_context_matches(import_lines, candidate);
    });
    auto imported_outcome = choose_unique_candidate(imported, "import_context_match", "candidate namespace appears in import/include/using context", 0.82);
    if (!imported_outcome.unresolved || !imported.empty()) {
        return imported_outcome;
    }

    auto global_name = collect([&](const ResolverSymbol&) {
        return true;
    });
    auto global_outcome = choose_unique_candidate(global_name, "unique_global_name", "only one active candidate has this simple name", 0.75);
    if (!global_outcome.unresolved || !global_name.empty()) {
        return global_outcome;
    }

    auto suffix = collect([&](const ResolverSymbol& candidate) {
        return qualified_suffix_matches(symbol_display_name(candidate), token.text);
    });
    auto suffix_outcome = choose_unique_candidate(suffix, "suffix_qualified_match", "token matched qualified-name suffix", 0.7);
    if (!suffix_outcome.unresolved || !suffix.empty()) {
        return suffix_outcome;
    }

    ResolutionOutcome unresolved;
    unresolved.target_text = token.text;
    unresolved.evidence = "resolver cascade found no deterministic candidate";
    return unresolved;
}
double parse_double_or_zero(const std::string& value)
{
    try {
        return value.empty() ? 0.0 : std::stod(value);
    } catch (const std::exception&) {
        return 0.0;
    }
}
std::string trim_copy(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) {
        return {};
    }
    return std::string{first, last};
}
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
        "source_file TEXT DEFAULT '',"
        "line INTEGER DEFAULT 0,"
        "column_number INTEGER DEFAULT 0,"
        "language TEXT DEFAULT '',"
        "resolution_strategy TEXT DEFAULT 'parser',"
        "is_unresolved INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "FOREIGN KEY(source_symbol_id) REFERENCES symbols(id),"
        "FOREIGN KEY(target_symbol_id) REFERENCES symbols(id)"
        ");");

    exec(
        "CREATE TABLE IF NOT EXISTS symbol_references ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "file_id INTEGER NOT NULL,"
        "source_symbol_id INTEGER,"
        "target_symbol_id INTEGER,"
        "source_file TEXT NOT NULL,"
        "line INTEGER NOT NULL,"
        "column_number INTEGER NOT NULL DEFAULT 0,"
        "reference_text TEXT NOT NULL,"
        "relationship_type TEXT NOT NULL,"
        "language TEXT NOT NULL,"
        "confidence REAL DEFAULT 0.0,"
        "resolution_strategy TEXT DEFAULT 'heuristic',"
        "is_unresolved INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "FOREIGN KEY(file_id) REFERENCES files(id),"
        "FOREIGN KEY(source_symbol_id) REFERENCES symbols(id),"
        "FOREIGN KEY(target_symbol_id) REFERENCES symbols(id)"
        ");");


    exec(
        "CREATE TABLE IF NOT EXISTS context_descriptions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "target_type TEXT NOT NULL,"
        "target_id INTEGER NOT NULL DEFAULT 0,"
        "target_key TEXT NOT NULL,"
        "description TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "UNIQUE(repository_id, target_type, target_id, target_key)"
        ");");

    exec(
        "CREATE INDEX IF NOT EXISTS idx_context_descriptions_target "
        "ON context_descriptions(repository_id, target_type, target_id, target_key);");

    exec(
        "CREATE TABLE IF NOT EXISTS virtual_files ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "signal_type TEXT NOT NULL,"
        "source_path TEXT NOT NULL,"
        "virtual_path TEXT NOT NULL,"
        "imported_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "content_hash TEXT NOT NULL,"
        "size_bytes INTEGER NOT NULL DEFAULT 0,"
        "line_count INTEGER NOT NULL DEFAULT 0,"
        "is_truncated INTEGER NOT NULL DEFAULT 0,"
        "content TEXT NOT NULL,"
        "is_active INTEGER NOT NULL DEFAULT 1,"
        "deleted_at TEXT,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "UNIQUE(repository_id, virtual_path)"
        ");");

    exec("CREATE INDEX IF NOT EXISTS idx_virtual_files_repo_type ON virtual_files(repository_id, signal_type, is_active);");
    exec("CREATE INDEX IF NOT EXISTS idx_virtual_files_repo_path ON virtual_files(repository_id, virtual_path);");
    exec("CREATE INDEX IF NOT EXISTS idx_virtual_files_hash ON virtual_files(repository_id, content_hash);");
    auto add_column_if_missing = [this](const std::string& table, const std::string& column_definition) {
        try {
            exec("ALTER TABLE " + table + " ADD COLUMN " + column_definition + ";");
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            if (message.find("duplicate column") == std::string::npos) {
                throw;
            }
        }
    };
    add_column_if_missing("symbol_relations", "source_file TEXT DEFAULT ''");
    add_column_if_missing("symbol_relations", "line INTEGER DEFAULT 0");
    add_column_if_missing("symbol_relations", "column_number INTEGER DEFAULT 0");
    add_column_if_missing("symbol_relations", "language TEXT DEFAULT ''");
    add_column_if_missing("symbol_relations", "resolution_strategy TEXT DEFAULT 'parser'");
    add_column_if_missing("symbol_relations", "is_unresolved INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing("symbol_relations", "resolution_evidence TEXT DEFAULT ''");
    add_column_if_missing("symbol_references", "resolution_evidence TEXT DEFAULT ''");


    exec(
        "CREATE TABLE IF NOT EXISTS scip_imports ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "source_path TEXT NOT NULL,"
        "imported_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "symbols_seen INTEGER NOT NULL DEFAULT 0,"
        "symbols_inserted INTEGER NOT NULL DEFAULT 0,"
        "symbols_mapped INTEGER NOT NULL DEFAULT 0,"
        "references_seen INTEGER NOT NULL DEFAULT 0,"
        "references_inserted INTEGER NOT NULL DEFAULT 0,"
        "relationships_inserted INTEGER NOT NULL DEFAULT 0,"
        "unresolved_references INTEGER NOT NULL DEFAULT 0,"
        "conflicts INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id)"
        ");");
    exec(
        "CREATE TABLE IF NOT EXISTS folder_fingerprints ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "repository_id INTEGER NOT NULL,"
        "folder_path TEXT NOT NULL,"
        "fingerprint TEXT NOT NULL,"
        "file_count INTEGER NOT NULL DEFAULT 0,"
        "total_size INTEGER NOT NULL DEFAULT 0,"
        "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "FOREIGN KEY(repository_id) REFERENCES repositories(id),"
        "UNIQUE(repository_id, folder_path)"
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
    exec("CREATE INDEX IF NOT EXISTS idx_files_structure_hash ON files(repository_id, structure_hash);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_name ON symbols(repository_id, name);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_qualified ON symbols(repository_id, qualified_name);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_repo_kind ON symbols(repository_id, kind);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_relations_source ON symbol_relations(source_symbol_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_relations_target ON symbol_relations(target_symbol_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_relations_type ON symbol_relations(repository_id, relation_type);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_references_target ON symbol_references(target_symbol_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_references_source ON symbol_references(source_symbol_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_symbol_references_unresolved ON symbol_references(repository_id, is_unresolved);");
    exec("CREATE INDEX IF NOT EXISTS idx_scip_imports_repository ON scip_imports(repository_id, imported_at);");
    exec("CREATE INDEX IF NOT EXISTS idx_folder_fingerprints_repository ON folder_fingerprints(repository_id, folder_path);");
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
        "SELECT id, relative_path, COALESCE(content_hash, ''), COALESCE(structure_hash, ''), is_active FROM files WHERE repository_id = ?;"};
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
        file.similarity_signature = column_string(impl_->api, statement.get(), 3);
        file.is_active = impl_->api.column_int(statement.get(), 4) != 0;
        files.emplace(file.relative_path, file);
    }

    return files;
}

std::unordered_map<std::string, FolderFingerprint> SqliteDatabase::read_folder_fingerprints(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT folder_path, fingerprint, file_count, total_size FROM folder_fingerprints WHERE repository_id = ? ORDER BY folder_path;"};
    statement.bind_int64(1, repository_id);

    std::unordered_map<std::string, FolderFingerprint> folders;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read folder fingerprints: "} + impl_->api.errmsg(impl_->database));
        }
        FolderFingerprint folder;
        folder.folder_path = column_string(impl_->api, statement.get(), 0);
        folder.fingerprint = column_string(impl_->api, statement.get(), 1);
        folder.file_count = impl_->api.column_int(statement.get(), 2);
        folder.total_size = impl_->api.column_int64(statement.get(), 3);
        folders.emplace(folder.folder_path, folder);
    }
    return folders;
}

void SqliteDatabase::replace_folder_fingerprints(long long repository_id, const std::vector<FolderFingerprint>& folders)
{
    Transaction transaction{impl_->api, impl_->database};
    Statement delete_statement{impl_->api, impl_->database, "DELETE FROM folder_fingerprints WHERE repository_id = ?;"};
    delete_statement.bind_int64(1, repository_id);
    delete_statement.step_done(impl_->database);

    Statement insert_statement{
        impl_->api,
        impl_->database,
        "INSERT INTO folder_fingerprints (repository_id, folder_path, fingerprint, file_count, total_size) VALUES (?, ?, ?, ?, ?);"};
    for (const auto& folder : folders) {
        insert_statement.bind_int64(1, repository_id);
        insert_statement.bind_text(2, folder.folder_path);
        insert_statement.bind_text(3, folder.fingerprint);
        insert_statement.bind_int(4, folder.file_count);
        insert_statement.bind_int64(5, folder.total_size);
        insert_statement.step_done(impl_->database);
        insert_statement.reset(impl_->database);
    }
    transaction.commit();
}

std::string SqliteDatabase::repository_fingerprint(long long repository_id)
{
    Statement statement{impl_->api, impl_->database, "SELECT COALESCE(repo_fingerprint, '') FROM repositories WHERE id = ?;"};
    statement.bind_int64(1, repository_id);
    const int result = impl_->api.step(statement.get());
    if (result == sqlite_done) {
        return {};
    }
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read repository fingerprint: "} + impl_->api.errmsg(impl_->database));
    }
    return column_string(impl_->api, statement.get(), 0);
}

void SqliteDatabase::update_repository_fingerprint(long long repository_id, const std::string& fingerprint)
{
    Statement statement{impl_->api, impl_->database, "UPDATE repositories SET repo_fingerprint = ? WHERE id = ?;"};
    statement.bind_text(1, fingerprint);
    statement.bind_int64(2, repository_id);
    statement.step_done(impl_->database);
}
std::vector<SimilarityFile> SqliteDatabase::read_similarity_files(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT relative_path, COALESCE(structure_hash, ''), COALESCE(language, ''), COALESCE(size_bytes, 0) "
        "FROM files WHERE repository_id = ? AND is_active = 1 AND COALESCE(structure_hash, '') <> '' "
        "ORDER BY structure_hash, relative_path;"};
    statement.bind_int64(1, repository_id);

    std::vector<SimilarityFile> files;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read similarity signatures: "} + impl_->api.errmsg(impl_->database));
        }
        SimilarityFile file;
        file.relative_path = column_string(impl_->api, statement.get(), 0);
        file.similarity_signature = column_string(impl_->api, statement.get(), 1);
        file.language = column_string(impl_->api, statement.get(), 2);
        file.size_bytes = impl_->api.column_int64(statement.get(), 3);
        files.push_back(file);
    }
    return files;
}
std::vector<IndexedFileFact> SqliteDatabase::active_files(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, relative_path, absolute_path, COALESCE(language, ''), COALESCE(size_bytes, 0), COALESCE(line_count, 0) "
        "FROM files WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
    statement.bind_int64(1, repository_id);

    std::vector<IndexedFileFact> files;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read active files: "} + impl_->api.errmsg(impl_->database));
        }

        IndexedFileFact file;
        file.file_id = impl_->api.column_int64(statement.get(), 0);
        file.relative_path = column_string(impl_->api, statement.get(), 1);
        file.absolute_path = column_string(impl_->api, statement.get(), 2);
        file.language = column_string(impl_->api, statement.get(), 3);
        file.size_bytes = impl_->api.column_int64(statement.get(), 4);
        file.line_count = impl_->api.column_int64(statement.get(), 5);
        files.push_back(file);
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

long long SqliteDatabase::upsert_file(
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
            "last_modified_time, content_hash, structure_hash, is_active, first_seen_snapshot_id, last_seen_snapshot_id"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?);"};

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
        return impl_->api.last_insert_rowid(impl_->database);
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE files SET "
        "absolute_path = ?, extension = ?, size_bytes = ?, line_count = ?, char_count = ?, language = ?, "
        "last_modified_time = ?, content_hash = ?, structure_hash = ?, is_active = 1, last_seen_snapshot_id = ?, deleted_at = NULL "
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

    Statement id_statement{
        impl_->api,
        impl_->database,
        "SELECT id FROM files WHERE repository_id = ? AND relative_path = ?;"};
    id_statement.bind_int64(1, repository_id);
    id_statement.bind_text(2, file.relative_path);
    const int result = impl_->api.step(id_statement.get());
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read updated file id: "} + impl_->api.errmsg(impl_->database));
    }
    return impl_->api.column_int64(id_statement.get(), 0);
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

ParseSaveStats SqliteDatabase::save_parse_result(long long repository_id, long long file_id, const ParseResult& result, bool lite_mode)
{
    ParseSaveStats stats;
    Transaction transaction{impl_->api, impl_->database};

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
        Statement statement{
            impl_->api,
            impl_->database,
            "DELETE FROM symbol_references WHERE file_id = ? "
            "OR source_symbol_id IN (SELECT id FROM symbols WHERE file_id = ?) "
            "OR target_symbol_id IN (SELECT id FROM symbols WHERE file_id = ?);"};
        statement.bind_int64(1, file_id);
        statement.bind_int64(2, file_id);
        statement.bind_int64(3, file_id);
        statement.step_done(impl_->database);
    }

    {
        Statement statement{impl_->api, impl_->database, "DELETE FROM symbols WHERE file_id = ?;"};
        statement.bind_int64(1, file_id);
        statement.step_done(impl_->database);
    }

    std::string source_relative_path;
    std::string source_absolute_path;
    {
        Statement file_path_statement{
            impl_->api,
            impl_->database,
            "SELECT relative_path, absolute_path FROM files WHERE id = ?;"};
        file_path_statement.bind_int64(1, file_id);
        const int file_path_result = impl_->api.step(file_path_statement.get());
        if (file_path_result == sqlite_row) {
            source_relative_path = column_string(impl_->api, file_path_statement.get(), 0);
            source_absolute_path = column_string(impl_->api, file_path_statement.get(), 1);
        }
    }

    std::vector<long long> inserted_symbol_ids;
    inserted_symbol_ids.reserve(result.symbols.size());

    Statement symbol_statement{
        impl_->api,
        impl_->database,
        "INSERT INTO symbols ("
        "repository_id, file_id, language, kind, name, qualified_name, signature, return_type, visibility, modifiers, "
        "parent_symbol_id, line_start, line_end, char_start, char_end, char_count, stable_id, description, tags, is_active"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, 0), ?, ?, ?, ?, ?, ?, '', '', 1);"};

    std::unique_ptr<Statement> parameter_statement;
    if (!lite_mode) {
        parameter_statement.reset(new Statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbol_parameters (symbol_id, name, type, default_value, position, direction) "
            "VALUES (?, ?, ?, ?, ?, ?);"});
    }

    for (const auto& symbol : result.symbols) {
        long long parent_symbol_id = 0;
        if (symbol.parent_index >= 0 && static_cast<std::size_t>(symbol.parent_index) < inserted_symbol_ids.size()) {
            parent_symbol_id = inserted_symbol_ids[static_cast<std::size_t>(symbol.parent_index)];
        }

        symbol_statement.bind_int64(1, repository_id);
        symbol_statement.bind_int64(2, file_id);
        symbol_statement.bind_text(3, result.language);
        symbol_statement.bind_text(4, symbol.kind);
        symbol_statement.bind_text(5, symbol.name);
        symbol_statement.bind_text(6, symbol.qualified_name);
        symbol_statement.bind_text(7, symbol.signature);
        symbol_statement.bind_text(8, symbol.return_type);
        symbol_statement.bind_text(9, symbol.visibility);
        symbol_statement.bind_text(10, symbol.modifiers);
        symbol_statement.bind_int64(11, parent_symbol_id);
        symbol_statement.bind_int(12, symbol.line_start);
        symbol_statement.bind_int(13, symbol.line_end);
        symbol_statement.bind_int(14, symbol.char_start);
        symbol_statement.bind_int(15, symbol.char_end);
        symbol_statement.bind_int(16, symbol.char_count);
        symbol_statement.bind_text(17, make_stable_symbol_id(source_relative_path, result.language, symbol.kind, symbol.qualified_name, symbol.name, symbol.signature, symbol.line_start));
        symbol_statement.step_done(impl_->database);

        const long long symbol_id = impl_->api.last_insert_rowid(impl_->database);
        symbol_statement.reset(impl_->database);
        inserted_symbol_ids.push_back(symbol_id);
        ++stats.symbols_inserted;

        if (!lite_mode) {
            for (const auto& parameter : symbol.parameters) {
                parameter_statement->bind_int64(1, symbol_id);
                parameter_statement->bind_text(2, parameter.name);
                parameter_statement->bind_text(3, parameter.type);
                parameter_statement->bind_text(4, parameter.default_value);
                parameter_statement->bind_int(5, parameter.position);
                parameter_statement->bind_text(6, parameter.direction);
                parameter_statement->step_done(impl_->database);
                parameter_statement->reset(impl_->database);
            }
        }
    }

    if (!lite_mode) {
        Statement relation_statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbol_relations ("
            "repository_id, source_symbol_id, target_symbol_id, relation_type, source_text, target_text, confidence, "
            "source_file, line, column_number, language, resolution_strategy, resolution_evidence, is_unresolved"
            ") VALUES (?, ?, NULLIF(?, 0), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"};

        auto insert_relation = [&](long long source_symbol_id,
                                   long long target_symbol_id,
                                   const std::string& relation_type,
                                   const std::string& source_text,
                                   const std::string& target_text,
                                   double confidence,
                                   int line,
                                   int column,
                                   const std::string& resolution_strategy,
                                   const std::string& resolution_evidence,
                                   bool unresolved) {
            relation_statement.bind_int64(1, repository_id);
            relation_statement.bind_int64(2, source_symbol_id);
            relation_statement.bind_int64(3, target_symbol_id);
            relation_statement.bind_text(4, relation_type);
            relation_statement.bind_text(5, source_text);
            relation_statement.bind_text(6, target_text);
            relation_statement.bind_text(7, std::to_string(confidence));
            relation_statement.bind_text(8, source_relative_path);
            relation_statement.bind_int(9, line);
            relation_statement.bind_int(10, column);
            relation_statement.bind_text(11, result.language);
            relation_statement.bind_text(12, resolution_strategy);
            relation_statement.bind_text(13, resolution_evidence);
            relation_statement.bind_int(14, unresolved ? 1 : 0);
            relation_statement.step_done(impl_->database);
            relation_statement.reset(impl_->database);
        };

        for (std::size_t index = 0; index < result.symbols.size(); ++index) {
            const auto& symbol = result.symbols[index];
            const long long source_symbol_id = inserted_symbol_ids[index];

            if (symbol.parent_index >= 0 && static_cast<std::size_t>(symbol.parent_index) < inserted_symbol_ids.size()) {
                const auto& parent = result.symbols[static_cast<std::size_t>(symbol.parent_index)];
                const long long parent_symbol_id = inserted_symbol_ids[static_cast<std::size_t>(symbol.parent_index)];
                insert_relation(parent_symbol_id, source_symbol_id, "contains", parent.qualified_name, symbol.qualified_name, 1.0, symbol.line_start, 1, "parser_parent", "parser parent_index established containment", false);
                insert_relation(source_symbol_id, parent_symbol_id, "contained_by", symbol.qualified_name, parent.qualified_name, 1.0, symbol.line_start, 1, "parser_parent", "parser parent_index established containment", false);
            }

            if ((symbol.kind == "method" || symbol.kind == "property") && is_relation_type_candidate(symbol.return_type)) {
                insert_relation(source_symbol_id, 0, "uses_type", symbol.qualified_name, symbol.return_type, 0.65, symbol.line_start, 1, "parser_type_text", "type text recorded without symbol resolution", true);
            }

            if (symbol.kind == "method" || symbol.kind == "constructor") {
                for (const auto& parameter : symbol.parameters) {
                    if (is_relation_type_candidate(parameter.type)) {
                        insert_relation(source_symbol_id, 0, "uses_type", symbol.qualified_name, parameter.type, 0.65, symbol.line_start, 1, "parser_type_text", "parameter type text recorded without symbol resolution", true);
                    }
                }
            }

            if (symbol.kind == "class" || symbol.kind == "interface" || symbol.kind == "struct") {
                for (std::size_t base_index = 0; base_index < symbol.base_types.size(); ++base_index) {
                    const auto relation_type = symbol.kind == "class" && base_index == 0 ? "inherits" : "implements";
                    insert_relation(source_symbol_id, 0, relation_type, symbol.qualified_name, symbol.base_types[base_index], 0.75, symbol.line_start, 1, "parser_base_type_text", "base type text recorded without symbol resolution", true);
                }
            }
        }

        Statement reference_statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbol_references ("
            "repository_id, file_id, source_symbol_id, target_symbol_id, source_file, line, column_number, "
            "reference_text, relationship_type, language, confidence, resolution_strategy, resolution_evidence, is_unresolved"
            ") VALUES (?, ?, NULLIF(?, 0), NULLIF(?, 0), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"};

        auto insert_reference = [&](long long source_symbol_id,
                                    long long target_symbol_id,
                                    int line,
                                    int column,
                                    const std::string& reference_text,
                                    const std::string& relationship_type,
                                    double confidence,
                                    const std::string& resolution_strategy,
                                    const std::string& resolution_evidence,
                                    bool unresolved) {
            reference_statement.bind_int64(1, repository_id);
            reference_statement.bind_int64(2, file_id);
            reference_statement.bind_int64(3, source_symbol_id);
            reference_statement.bind_int64(4, target_symbol_id);
            reference_statement.bind_text(5, source_relative_path);
            reference_statement.bind_int(6, line);
            reference_statement.bind_int(7, column);
            reference_statement.bind_text(8, reference_text);
            reference_statement.bind_text(9, relationship_type);
            reference_statement.bind_text(10, result.language);
            reference_statement.bind_text(11, std::to_string(confidence));
            reference_statement.bind_text(12, resolution_strategy);
            reference_statement.bind_text(13, resolution_evidence);
            reference_statement.bind_int(14, unresolved ? 1 : 0);
            reference_statement.step_done(impl_->database);
            reference_statement.reset(impl_->database);
        };

        std::vector<ResolverSymbol> resolver_symbols;
        resolver_symbols.reserve(result.symbols.size());
        for (std::size_t index = 0; index < result.symbols.size(); ++index) {
            const auto& symbol = result.symbols[index];
            ResolverSymbol item;
            item.id = inserted_symbol_ids[index];
            item.local_index = index;
            item.local = true;
            item.file_path = source_relative_path;
            item.language = result.language;
            item.kind = symbol.kind;
            item.name = symbol.name;
            item.qualified_name = symbol.qualified_name;
            item.line_start = symbol.line_start;
            item.line_end = symbol.line_end;
            if (symbol.parent_index >= 0 && static_cast<std::size_t>(symbol.parent_index) < inserted_symbol_ids.size()) {
                item.parent_id = inserted_symbol_ids[static_cast<std::size_t>(symbol.parent_index)];
                const auto& parent = result.symbols[static_cast<std::size_t>(symbol.parent_index)];
                item.parent_qualified_name = parent.qualified_name.empty() ? parent.name : parent.qualified_name;
            }
            resolver_symbols.push_back(item);
        }

        {
            Statement active_symbol_statement{
                impl_->api,
                impl_->database,
                "SELECT s.id, COALESCE(s.parent_symbol_id, 0), f.relative_path, COALESCE(s.language, f.language, ''), "
                "s.kind, s.name, COALESCE(s.qualified_name, ''), COALESCE(parent.qualified_name, parent.name, ''), "
                "COALESCE(s.line_start, 0), COALESCE(s.line_end, 0) "
                "FROM symbols s JOIN files f ON f.id = s.file_id "
                "LEFT JOIN symbols parent ON parent.id = s.parent_symbol_id "
                "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
                "ORDER BY f.relative_path, COALESCE(s.line_start, 0), s.kind, s.qualified_name;"};
            active_symbol_statement.bind_int64(1, repository_id);
            while (true) {
                const int step_result = impl_->api.step(active_symbol_statement.get());
                if (step_result == sqlite_done) {
                    break;
                }
                if (step_result != sqlite_row) {
                    throw std::runtime_error(std::string{"Failed to read resolver candidates: "} + impl_->api.errmsg(impl_->database));
                }
                const long long candidate_id = impl_->api.column_int64(active_symbol_statement.get(), 0);
                if (std::find(inserted_symbol_ids.begin(), inserted_symbol_ids.end(), candidate_id) != inserted_symbol_ids.end()) {
                    continue;
                }
                ResolverSymbol item;
                item.id = candidate_id;
                item.parent_id = impl_->api.column_int64(active_symbol_statement.get(), 1);
                item.file_path = column_string(impl_->api, active_symbol_statement.get(), 2);
                item.language = column_string(impl_->api, active_symbol_statement.get(), 3);
                item.kind = column_string(impl_->api, active_symbol_statement.get(), 4);
                item.name = column_string(impl_->api, active_symbol_statement.get(), 5);
                item.qualified_name = column_string(impl_->api, active_symbol_statement.get(), 6);
                item.parent_qualified_name = column_string(impl_->api, active_symbol_statement.get(), 7);
                item.line_start = impl_->api.column_int(active_symbol_statement.get(), 8);
                item.line_end = impl_->api.column_int(active_symbol_statement.get(), 9);
                resolver_symbols.push_back(item);
            }
        }

        const auto source_lines = read_source_lines(source_absolute_path);
        const auto type_aliases = collect_type_aliases(source_lines);
        auto source_symbol_index_for_line = [&](int line_number) {
            std::size_t best_index = static_cast<std::size_t>(-1);
            int best_span = 2147483647;
            for (std::size_t index = 0; index < result.symbols.size(); ++index) {
                const auto& symbol = result.symbols[index];
                if (line_number >= symbol.line_start && line_number <= symbol.line_end &&
                    (symbol.kind == "function" || symbol.kind == "method" || symbol.kind == "constructor" || symbol.kind == "property")) {
                    const int span = symbol.line_end - symbol.line_start;
                    if (span < best_span) {
                        best_span = span;
                        best_index = index;
                    }
                }
            }
            return best_index;
        };

        std::vector<std::string> import_lines;
        for (std::size_t line_index = 0; line_index < source_lines.size(); ++line_index) {
            const int line_number = static_cast<int>(line_index + 1);
            const auto trimmed = trim_copy(source_lines[line_index]);
            if (trimmed.rfind("#include", 0) == 0) {
                import_lines.push_back(trimmed);
                insert_reference(0, 0, line_number, 1, trimmed, "includes", 0.8, "preprocessor_include", "include/import inventory; not resolved to symbol", true);
                continue;
            }
            if (trimmed.rfind("using ", 0) == 0 || trimmed.rfind("import ", 0) == 0) {
                import_lines.push_back(trimmed);
                insert_reference(0, 0, line_number, 1, trimmed, "imports", 0.75, "import_statement", "using/import inventory; used as resolver context", true);
            }
        }

        for (std::size_t line_index = 0; line_index < source_lines.size(); ++line_index) {
            const int line_number = static_cast<int>(line_index + 1);
            const auto source_index = source_symbol_index_for_line(line_number);
            if (source_index == static_cast<std::size_t>(-1)) {
                continue;
            }
            const auto& source = result.symbols[source_index];
            if (line_number == source.line_start) {
                continue;
            }
            const long long source_symbol_id = inserted_symbol_ids[source_index];
            const auto source_candidate = std::find_if(resolver_symbols.begin(), resolver_symbols.end(), [&](const ResolverSymbol& item) {
                return item.id == source_symbol_id;
            });
            if (source_candidate == resolver_symbols.end()) {
                continue;
            }

            const auto tokens = extract_call_tokens(source_lines[line_index], line_number);
            for (const auto& token : tokens) {
                const auto receiver_type = infer_receiver_type(result, source_index, source_lines, type_aliases, token.receiver, line_number);
                const auto outcome = resolve_reference_token(token, resolver_symbols, *source_candidate, import_lines, receiver_type);
                const auto relationship_type = token.call ? std::string{"calls"} : std::string{"references"};
                const auto target_text = outcome.target_text.empty() ? token.text : outcome.target_text;
                insert_reference(
                    source_symbol_id,
                    outcome.target_symbol_id,
                    token.line,
                    token.column,
                    token.text,
                    relationship_type,
                    outcome.confidence,
                    outcome.strategy,
                    outcome.evidence,
                    outcome.unresolved);
                insert_relation(
                    source_symbol_id,
                    outcome.target_symbol_id,
                    relationship_type,
                    symbol_display_name(*source_candidate),
                    target_text,
                    outcome.confidence,
                    token.line,
                    token.column,
                    outcome.strategy,
                    outcome.evidence,
                    outcome.unresolved);
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
    transaction.commit();
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
    counts.symbol_references = count_table_rows(impl_->api, impl_->database, "symbol_references");
    counts.context_descriptions = count_table_rows(impl_->api, impl_->database, "context_descriptions");
    counts.virtual_files = count_table_rows(impl_->api, impl_->database, "virtual_files");
    counts.scip_imports = count_table_rows(impl_->api, impl_->database, "scip_imports");
    counts.snapshots = count_table_rows(impl_->api, impl_->database, "snapshots");
    counts.changes = count_table_rows(impl_->api, impl_->database, "changes");
    return counts;
}

void SqliteDatabase::prune_lite_metadata()
{
    Transaction transaction{impl_->api, impl_->database};
    exec("DELETE FROM symbol_parameters;");
    exec("DELETE FROM symbol_relations;");
    exec("DELETE FROM symbol_references;");
    exec("DELETE FROM changes;");
    exec("DELETE FROM snapshots;");
    transaction.commit();
}

void SqliteDatabase::compact()
{
    exec("PRAGMA optimize;");
    exec("VACUUM;");
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
VirtualFileFact SqliteDatabase::upsert_virtual_file(long long repository_id, const VirtualFileInput& input)
{
    if (input.signal_type.empty()) {
        throw std::runtime_error("Signal type is required.");
    }
    if (input.virtual_path.empty()) {
        throw std::runtime_error("Virtual signal path is required.");
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "INSERT INTO virtual_files "
        "(repository_id, signal_type, source_path, virtual_path, imported_at, content_hash, size_bytes, line_count, is_truncated, content, is_active, deleted_at) "
        "VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP, ?, ?, ?, ?, ?, 1, NULL) "
        "ON CONFLICT(repository_id, virtual_path) DO UPDATE SET "
        "signal_type = excluded.signal_type, source_path = excluded.source_path, imported_at = CURRENT_TIMESTAMP, "
        "content_hash = excluded.content_hash, size_bytes = excluded.size_bytes, line_count = excluded.line_count, "
        "is_truncated = excluded.is_truncated, content = excluded.content, is_active = 1, deleted_at = NULL;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, input.signal_type);
    statement.bind_text(3, input.source_path);
    statement.bind_text(4, input.virtual_path);
    statement.bind_text(5, input.content_hash);
    statement.bind_int64(6, input.size_bytes);
    statement.bind_int64(7, input.line_count);
    statement.bind_int(8, input.truncated ? 1 : 0);
    statement.bind_text(9, input.content);
    statement.step_done(impl_->database);

    Statement read_statement{
        impl_->api,
        impl_->database,
        "SELECT id, signal_type, source_path, virtual_path, imported_at, content_hash, size_bytes, line_count, is_truncated "
        "FROM virtual_files WHERE repository_id = ? AND virtual_path = ? ORDER BY id LIMIT 1;"};
    read_statement.bind_int64(1, repository_id);
    read_statement.bind_text(2, input.virtual_path);
    if (impl_->api.step(read_statement.get()) != sqlite_row) {
        throw std::runtime_error("Failed to read imported virtual file.");
    }

    VirtualFileFact file;
    file.id = impl_->api.column_int64(read_statement.get(), 0);
    file.signal_type = column_string(impl_->api, read_statement.get(), 1);
    file.source_path = column_string(impl_->api, read_statement.get(), 2);
    file.virtual_path = column_string(impl_->api, read_statement.get(), 3);
    file.imported_at = column_string(impl_->api, read_statement.get(), 4);
    file.content_hash = column_string(impl_->api, read_statement.get(), 5);
    file.size_bytes = impl_->api.column_int64(read_statement.get(), 6);
    file.line_count = impl_->api.column_int64(read_statement.get(), 7);
    file.truncated = impl_->api.column_int(read_statement.get(), 8) != 0;
    return file;
}

std::vector<VirtualFileFact> SqliteDatabase::list_virtual_files(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, signal_type, source_path, virtual_path, imported_at, content_hash, size_bytes, line_count, is_truncated "
        "FROM virtual_files WHERE repository_id = ? AND is_active = 1 "
        "ORDER BY signal_type, virtual_path, id;"};
    statement.bind_int64(1, repository_id);

    std::vector<VirtualFileFact> files;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to list virtual files: "} + impl_->api.errmsg(impl_->database));
        }

        VirtualFileFact file;
        file.id = impl_->api.column_int64(statement.get(), 0);
        file.signal_type = column_string(impl_->api, statement.get(), 1);
        file.source_path = column_string(impl_->api, statement.get(), 2);
        file.virtual_path = column_string(impl_->api, statement.get(), 3);
        file.imported_at = column_string(impl_->api, statement.get(), 4);
        file.content_hash = column_string(impl_->api, statement.get(), 5);
        file.size_bytes = impl_->api.column_int64(statement.get(), 6);
        file.line_count = impl_->api.column_int64(statement.get(), 7);
        file.truncated = impl_->api.column_int(statement.get(), 8) != 0;
        files.push_back(file);
    }
    return files;
}

std::vector<VirtualFileSearchResult> SqliteDatabase::search_virtual_files(long long repository_id, const std::string& query, int limit)
{
    std::vector<VirtualFileSearchResult> results;
    if (query.empty()) {
        return results;
    }
    const int bounded_limit = limit <= 0 ? 20 : limit;
    const std::string pattern = "%" + query + "%";
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, signal_type, source_path, virtual_path, imported_at, content_hash, size_bytes, line_count, is_truncated, content "
        "FROM virtual_files WHERE repository_id = ? AND is_active = 1 "
        "AND (virtual_path LIKE ? OR source_path LIKE ? OR content LIKE ?) "
        "ORDER BY signal_type, virtual_path, id LIMIT ?;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, pattern);
    statement.bind_text(3, pattern);
    statement.bind_text(4, pattern);
    statement.bind_int(5, bounded_limit);

    const auto lower_query = ascii_lower_copy(query);
    while (results.size() < static_cast<std::size_t>(bounded_limit)) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to search virtual files: "} + impl_->api.errmsg(impl_->database));
        }

        VirtualFileSearchResult item;
        item.file.id = impl_->api.column_int64(statement.get(), 0);
        item.file.signal_type = column_string(impl_->api, statement.get(), 1);
        item.file.source_path = column_string(impl_->api, statement.get(), 2);
        item.file.virtual_path = column_string(impl_->api, statement.get(), 3);
        item.file.imported_at = column_string(impl_->api, statement.get(), 4);
        item.file.content_hash = column_string(impl_->api, statement.get(), 5);
        item.file.size_bytes = impl_->api.column_int64(statement.get(), 6);
        item.file.line_count = impl_->api.column_int64(statement.get(), 7);
        item.file.truncated = impl_->api.column_int(statement.get(), 8) != 0;
        const auto content = column_string(impl_->api, statement.get(), 9);

        std::istringstream stream{content};
        std::string line;
        int line_number = 0;
        while (std::getline(stream, line)) {
            ++line_number;
            if (ascii_lower_copy(line).find(lower_query) != std::string::npos) {
                item.line = line_number;
                item.snippet = line.size() > 240 ? line.substr(0, 240) : line;
                break;
            }
        }
        if (item.line == 0) {
            item.line = 1;
            item.snippet = content.size() > 240 ? content.substr(0, 240) : content;
        }
        results.push_back(item);
    }
    return results;
}

bool SqliteDatabase::delete_virtual_file(long long repository_id, const std::string& virtual_path_or_source_path)
{
    Statement exists_statement{
        impl_->api,
        impl_->database,
        "SELECT id FROM virtual_files WHERE repository_id = ? AND is_active = 1 AND (virtual_path = ? OR source_path = ?) ORDER BY id LIMIT 1;"};
    exists_statement.bind_int64(1, repository_id);
    exists_statement.bind_text(2, virtual_path_or_source_path);
    exists_statement.bind_text(3, virtual_path_or_source_path);
    const int exists_result = impl_->api.step(exists_statement.get());
    if (exists_result == sqlite_done) {
        return false;
    }
    if (exists_result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to find virtual file: "} + impl_->api.errmsg(impl_->database));
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "UPDATE virtual_files SET is_active = 0, deleted_at = CURRENT_TIMESTAMP "
        "WHERE repository_id = ? AND is_active = 1 AND (virtual_path = ? OR source_path = ?);"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, virtual_path_or_source_path);
    statement.bind_text(3, virtual_path_or_source_path);
    statement.step_done(impl_->database);
    return true;
}

std::vector<FactSymbol> SqliteDatabase::resolve_symbols(
    long long repository_id,
    const std::string& symbol_name,
    const std::optional<std::string>& file_path)
{
    std::string matched_file_path;
    if (file_path && !file_path->empty()) {
        const auto requested_path = normalize_fact_path(*file_path);
        Statement file_statement{
            impl_->api,
            impl_->database,
            "SELECT relative_path, absolute_path FROM files "
            "WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
        file_statement.bind_int64(1, repository_id);

        while (true) {
            const int result = impl_->api.step(file_statement.get());
            if (result == sqlite_done) {
                break;
            }
            if (result != sqlite_row) {
                throw std::runtime_error(std::string{"Failed to resolve file path: "} + impl_->api.errmsg(impl_->database));
            }

            const auto relative_path = column_string(impl_->api, file_statement.get(), 0);
            const auto absolute_path = column_string(impl_->api, file_statement.get(), 1);
            if (normalize_fact_path(relative_path) == requested_path || normalize_fact_path(absolute_path) == requested_path) {
                matched_file_path = relative_path;
                break;
            }
        }

        if (matched_file_path.empty()) {
            throw std::runtime_error("File path not found: " + *file_path);
        }
    }

    const bool has_file_filter = !matched_file_path.empty();
    Statement statement{
        impl_->api,
        impl_->database,
        has_file_filter
            ? "SELECT s.id, COALESCE(s.stable_id, ''), f.relative_path, f.absolute_path, "
              "COALESCE(s.language, f.language, ''), s.kind, s.name, COALESCE(s.qualified_name, ''), "
              "COALESCE(s.signature, ''), COALESCE(s.line_start, 0), COALESCE(s.line_end, 0), "
              "COALESCE(parent.qualified_name, parent.name, '') "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "LEFT JOIN symbols parent ON parent.id = s.parent_symbol_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "AND (s.name = ? OR COALESCE(s.qualified_name, '') = ?) AND f.relative_path = ? "
              "ORDER BY CASE WHEN s.name = ? THEN 0 ELSE 1 END, f.relative_path, COALESCE(s.line_start, 0), s.kind, s.qualified_name;"
            : "SELECT s.id, COALESCE(s.stable_id, ''), f.relative_path, f.absolute_path, "
              "COALESCE(s.language, f.language, ''), s.kind, s.name, COALESCE(s.qualified_name, ''), "
              "COALESCE(s.signature, ''), COALESCE(s.line_start, 0), COALESCE(s.line_end, 0), "
              "COALESCE(parent.qualified_name, parent.name, '') "
              "FROM symbols s JOIN files f ON f.id = s.file_id "
              "LEFT JOIN symbols parent ON parent.id = s.parent_symbol_id "
              "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 "
              "AND (s.name = ? OR COALESCE(s.qualified_name, '') = ?) "
              "ORDER BY CASE WHEN s.name = ? THEN 0 ELSE 1 END, f.relative_path, COALESCE(s.line_start, 0), s.kind, s.qualified_name;"};

    statement.bind_int64(1, repository_id);
    statement.bind_text(2, symbol_name);
    statement.bind_text(3, symbol_name);
    if (has_file_filter) {
        statement.bind_text(4, matched_file_path);
        statement.bind_text(5, symbol_name);
    } else {
        statement.bind_text(4, symbol_name);
    }

    std::vector<FactSymbol> results;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to resolve symbols: "} + impl_->api.errmsg(impl_->database));
        }

        FactSymbol item;
        item.row_id = impl_->api.column_int64(statement.get(), 0);
        item.stable_id = column_string(impl_->api, statement.get(), 1);
        item.file_path = column_string(impl_->api, statement.get(), 2);
        item.absolute_path = column_string(impl_->api, statement.get(), 3);
        item.language = column_string(impl_->api, statement.get(), 4);
        item.kind = column_string(impl_->api, statement.get(), 5);
        item.name = column_string(impl_->api, statement.get(), 6);
        item.qualified_name = column_string(impl_->api, statement.get(), 7);
        item.signature = column_string(impl_->api, statement.get(), 8);
        item.line_start = impl_->api.column_int(statement.get(), 9);
        item.line_end = impl_->api.column_int(statement.get(), 10);
        item.parent_scope = column_string(impl_->api, statement.get(), 11);
        if (item.stable_id.empty()) {
            item.stable_id = make_stable_symbol_id(
                item.file_path,
                item.language,
                item.kind,
                item.qualified_name,
                item.name,
                item.signature,
                item.line_start);
        }
        results.push_back(item);
    }

    return results;
}

std::vector<FactSymbol> SqliteDatabase::symbols_for_file(long long repository_id, const std::string& file_path)
{
    std::string matched_file_path;
    const auto requested_path = normalize_fact_path(file_path);
    Statement file_statement{
        impl_->api,
        impl_->database,
        "SELECT relative_path, absolute_path FROM files "
        "WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
    file_statement.bind_int64(1, repository_id);

    while (true) {
        const int result = impl_->api.step(file_statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to resolve file path: "} + impl_->api.errmsg(impl_->database));
        }

        const auto relative_path = column_string(impl_->api, file_statement.get(), 0);
        const auto absolute_path = column_string(impl_->api, file_statement.get(), 1);
        if (normalize_fact_path(relative_path) == requested_path || normalize_fact_path(absolute_path) == requested_path) {
            matched_file_path = relative_path;
            break;
        }
    }

    if (matched_file_path.empty()) {
        throw std::runtime_error("File path not found: " + file_path);
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT s.id, COALESCE(s.parent_symbol_id, 0), COALESCE(s.stable_id, ''), f.relative_path, f.absolute_path, "
        "COALESCE(s.language, f.language, ''), s.kind, s.name, COALESCE(s.qualified_name, ''), "
        "COALESCE(s.signature, ''), COALESCE(s.visibility, ''), COALESCE(s.line_start, 0), COALESCE(s.line_end, 0), "
        "COALESCE(parent.qualified_name, parent.name, '') "
        "FROM symbols s JOIN files f ON f.id = s.file_id "
        "LEFT JOIN symbols parent ON parent.id = s.parent_symbol_id "
        "WHERE s.repository_id = ? AND s.is_active = 1 AND f.is_active = 1 AND f.relative_path = ? "
        "ORDER BY COALESCE(s.line_start, 0), COALESCE(s.line_end, 0), s.kind, s.qualified_name;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, matched_file_path);

    std::vector<FactSymbol> results;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read file symbols: "} + impl_->api.errmsg(impl_->database));
        }

        FactSymbol item;
        item.row_id = impl_->api.column_int64(statement.get(), 0);
        item.parent_row_id = impl_->api.column_int64(statement.get(), 1);
        item.stable_id = column_string(impl_->api, statement.get(), 2);
        item.file_path = column_string(impl_->api, statement.get(), 3);
        item.absolute_path = column_string(impl_->api, statement.get(), 4);
        item.language = column_string(impl_->api, statement.get(), 5);
        item.kind = column_string(impl_->api, statement.get(), 6);
        item.name = column_string(impl_->api, statement.get(), 7);
        item.qualified_name = column_string(impl_->api, statement.get(), 8);
        item.signature = column_string(impl_->api, statement.get(), 9);
        item.visibility = column_string(impl_->api, statement.get(), 10);
        item.line_start = impl_->api.column_int(statement.get(), 11);
        item.line_end = impl_->api.column_int(statement.get(), 12);
        item.parent_scope = column_string(impl_->api, statement.get(), 13);
        if (item.stable_id.empty()) {
            item.stable_id = make_stable_symbol_id(
                item.file_path,
                item.language,
                item.kind,
                item.qualified_name,
                item.name,
                item.signature,
                item.line_start);
        }
        results.push_back(item);
    }

    return results;
}
SourceSnippet SqliteDatabase::read_snippet(long long repository_id, const std::string& file_path, int line_start, int line_end)
{
    if (line_start < 1) {
        throw std::runtime_error("Invalid line range: start line must be at least 1.");
    }
    if (line_end < line_start) {
        throw std::runtime_error("Invalid line range: end line is before start line.");
    }

    SourceSnippet snippet;
    long long indexed_line_count = 0;
    const auto requested_path = normalize_fact_path(file_path);
    Statement file_statement{
        impl_->api,
        impl_->database,
        "SELECT relative_path, absolute_path, COALESCE(language, ''), COALESCE(line_count, 0) FROM files "
        "WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
    file_statement.bind_int64(1, repository_id);

    while (true) {
        const int result = impl_->api.step(file_statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read indexed files: "} + impl_->api.errmsg(impl_->database));
        }

        const auto relative_path = column_string(impl_->api, file_statement.get(), 0);
        const auto absolute_path = column_string(impl_->api, file_statement.get(), 1);
        if (normalize_fact_path(relative_path) == requested_path || normalize_fact_path(absolute_path) == requested_path) {
            snippet.file_path = relative_path;
            snippet.absolute_path = absolute_path;
            snippet.language = column_string(impl_->api, file_statement.get(), 2);
            indexed_line_count = impl_->api.column_int64(file_statement.get(), 3);
            break;
        }
    }

    if (snippet.file_path.empty()) {
        throw std::runtime_error("File path not found: " + file_path);
    }
    if (indexed_line_count > 0 && line_end > indexed_line_count) {
        throw std::runtime_error("Invalid line range: end line exceeds indexed file length.");
    }

    std::ifstream source{snippet.absolute_path};
    if (!source) {
        throw std::runtime_error("Failed to read source file: " + snippet.absolute_path);
    }

    std::ostringstream code;
    std::string line;
    int line_number = 1;
    int copied_lines = 0;
    while (std::getline(source, line)) {
        if (line_number >= line_start && line_number <= line_end) {
            code << line << '\n';
            ++copied_lines;
        }
        if (line_number >= line_end) {
            break;
        }
        ++line_number;
    }

    const int expected_lines = line_end - line_start + 1;
    if (copied_lines != expected_lines) {
        throw std::runtime_error("Invalid line range: requested lines are outside the source file.");
    }

    snippet.line_start = line_start;
    snippet.line_end = line_end;
    snippet.code = code.str();
    return snippet;
}
void SqliteDatabase::upsert_context_description(long long repository_id, const ContextDescription& description)
{
    if (description.target_type.empty()) {
        throw std::runtime_error("Context description target_type is required.");
    }
    if (description.description.empty()) {
        throw std::runtime_error("Context description text is required.");
    }

    Statement statement{
        impl_->api,
        impl_->database,
        "INSERT INTO context_descriptions "
        "(repository_id, target_type, target_id, target_key, description, source, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
        "ON CONFLICT(repository_id, target_type, target_id, target_key) DO UPDATE SET "
        "description = excluded.description, source = excluded.source, updated_at = CURRENT_TIMESTAMP;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, description.target_type);
    statement.bind_int64(3, description.target_id);
    statement.bind_text(4, description.target_key);
    statement.bind_text(5, description.description);
    statement.bind_text(6, description.source.empty() ? std::string{"deterministic"} : description.source);

    const int result = impl_->api.step(statement.get());
    if (result != sqlite_done) {
        throw std::runtime_error(std::string{"Failed to upsert context description: "} + impl_->api.errmsg(impl_->database));
    }
}

std::optional<ContextDescription> SqliteDatabase::context_description_for(
    long long repository_id,
    const std::string& target_type,
    long long target_id,
    const std::string& target_key)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, target_type, target_id, target_key, description, source, updated_at "
        "FROM context_descriptions "
        "WHERE repository_id = ? AND target_type = ? AND target_id = ? AND target_key = ? "
        "ORDER BY id LIMIT 1;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, target_type);
    statement.bind_int64(3, target_id);
    statement.bind_text(4, target_key);

    const int result = impl_->api.step(statement.get());
    if (result == sqlite_done) {
        return std::nullopt;
    }
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read context description: "} + impl_->api.errmsg(impl_->database));
    }

    ContextDescription item;
    item.id = impl_->api.column_int64(statement.get(), 0);
    item.target_type = column_string(impl_->api, statement.get(), 1);
    item.target_id = impl_->api.column_int64(statement.get(), 2);
    item.target_key = column_string(impl_->api, statement.get(), 3);
    item.description = column_string(impl_->api, statement.get(), 4);
    item.source = column_string(impl_->api, statement.get(), 5);
    item.updated_at = column_string(impl_->api, statement.get(), 6);
    return item;
}

std::vector<ContextDescription> SqliteDatabase::context_descriptions(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT id, target_type, target_id, target_key, description, source, updated_at "
        "FROM context_descriptions WHERE repository_id = ? "
        "ORDER BY target_type, target_key, target_id, id;"};
    statement.bind_int64(1, repository_id);

    std::vector<ContextDescription> results;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to list context descriptions: "} + impl_->api.errmsg(impl_->database));
        }

        ContextDescription item;
        item.id = impl_->api.column_int64(statement.get(), 0);
        item.target_type = column_string(impl_->api, statement.get(), 1);
        item.target_id = impl_->api.column_int64(statement.get(), 2);
        item.target_key = column_string(impl_->api, statement.get(), 3);
        item.description = column_string(impl_->api, statement.get(), 4);
        item.source = column_string(impl_->api, statement.get(), 5);
        item.updated_at = column_string(impl_->api, statement.get(), 6);
        results.push_back(item);
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

std::vector<SymbolReferenceFact> SqliteDatabase::references_for_symbol(long long repository_id, const std::string& symbol_name)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT r.id, COALESCE(r.source_symbol_id, 0), COALESCE(r.target_symbol_id, 0), "
        "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.reference_text), "
        "r.source_file, r.language, r.line, r.column_number, r.reference_text, r.relationship_type, "
        "COALESCE(r.confidence, 0), r.resolution_strategy, COALESCE(r.resolution_evidence, ''), r.is_unresolved "
        "FROM symbol_references r "
        "LEFT JOIN symbols source ON source.id = r.source_symbol_id "
        "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
        "WHERE r.repository_id = ? AND (target.name = ? OR COALESCE(target.qualified_name, '') = ? OR r.reference_text = ?) "
        "ORDER BY r.source_file, r.line, r.column_number, r.relationship_type, r.reference_text;"};
    statement.bind_int64(1, repository_id);
    statement.bind_text(2, symbol_name);
    statement.bind_text(3, symbol_name);
    statement.bind_text(4, symbol_name);

    std::vector<SymbolReferenceFact> references;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read symbol references: "} + impl_->api.errmsg(impl_->database));
        }

        SymbolReferenceFact item;
        item.reference_id = impl_->api.column_int64(statement.get(), 0);
        item.source_symbol_id = impl_->api.column_int64(statement.get(), 1);
        item.target_symbol_id = impl_->api.column_int64(statement.get(), 2);
        item.source_symbol = column_string(impl_->api, statement.get(), 3);
        item.target_symbol = column_string(impl_->api, statement.get(), 4);
        item.source_file = column_string(impl_->api, statement.get(), 5);
        item.language = column_string(impl_->api, statement.get(), 6);
        item.line = impl_->api.column_int(statement.get(), 7);
        item.column = impl_->api.column_int(statement.get(), 8);
        item.reference_text = column_string(impl_->api, statement.get(), 9);
        item.relationship_type = column_string(impl_->api, statement.get(), 10);
        item.confidence = parse_double_or_zero(column_string(impl_->api, statement.get(), 11));
        item.resolution_strategy = column_string(impl_->api, statement.get(), 12);
        item.resolution_evidence = column_string(impl_->api, statement.get(), 13);
        item.unresolved = impl_->api.column_int(statement.get(), 14) != 0;
        references.push_back(item);
    }
    return references;
}

std::vector<SymbolRelationshipFact> SqliteDatabase::relationships_for_symbol(
    long long repository_id,
    const std::string& symbol_name,
    const std::optional<std::string>& relationship_type)
{
    const bool has_type = relationship_type && !relationship_type->empty();
    Statement statement{
        impl_->api,
        impl_->database,
        has_type
            ? "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
              "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text), "
              "COALESCE(r.source_file, ''), COALESCE(r.language, ''), COALESCE(r.line, 0), COALESCE(r.column_number, 0), "
              "r.relation_type, COALESCE(r.target_text, r.source_text, ''), COALESCE(r.confidence, 0), "
              "COALESCE(r.resolution_strategy, ''), COALESCE(r.resolution_evidence, ''), COALESCE(r.is_unresolved, 0) "
              "FROM symbol_relations r JOIN symbols source ON source.id = r.source_symbol_id "
              "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
              "WHERE r.repository_id = ? AND r.relation_type = ? "
              "AND (source.name = ? OR COALESCE(source.qualified_name, '') = ? OR target.name = ? OR COALESCE(target.qualified_name, '') = ?) "
              "ORDER BY r.relation_type, r.source_file, r.line, r.column_number, source.qualified_name, target.qualified_name;"
            : "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
              "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text), "
              "COALESCE(r.source_file, ''), COALESCE(r.language, ''), COALESCE(r.line, 0), COALESCE(r.column_number, 0), "
              "r.relation_type, COALESCE(r.target_text, r.source_text, ''), COALESCE(r.confidence, 0), "
              "COALESCE(r.resolution_strategy, ''), COALESCE(r.resolution_evidence, ''), COALESCE(r.is_unresolved, 0) "
              "FROM symbol_relations r JOIN symbols source ON source.id = r.source_symbol_id "
              "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
              "WHERE r.repository_id = ? "
              "AND (source.name = ? OR COALESCE(source.qualified_name, '') = ? OR target.name = ? OR COALESCE(target.qualified_name, '') = ?) "
              "ORDER BY r.relation_type, r.source_file, r.line, r.column_number, source.qualified_name, target.qualified_name;"};
    int bind_index = 1;
    statement.bind_int64(bind_index++, repository_id);
    if (has_type) {
        statement.bind_text(bind_index++, *relationship_type);
    }
    statement.bind_text(bind_index++, symbol_name);
    statement.bind_text(bind_index++, symbol_name);
    statement.bind_text(bind_index++, symbol_name);
    statement.bind_text(bind_index++, symbol_name);

    std::vector<SymbolRelationshipFact> relationships;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read symbol relationships: "} + impl_->api.errmsg(impl_->database));
        }

        SymbolRelationshipFact item;
        item.relationship_id = impl_->api.column_int64(statement.get(), 0);
        item.source_symbol_id = impl_->api.column_int64(statement.get(), 1);
        item.target_symbol_id = impl_->api.column_int64(statement.get(), 2);
        item.source_symbol = column_string(impl_->api, statement.get(), 3);
        item.target_symbol = column_string(impl_->api, statement.get(), 4);
        item.source_file = column_string(impl_->api, statement.get(), 5);
        item.language = column_string(impl_->api, statement.get(), 6);
        item.line = impl_->api.column_int(statement.get(), 7);
        item.column = impl_->api.column_int(statement.get(), 8);
        item.relationship_type = column_string(impl_->api, statement.get(), 9);
        item.reference_text = column_string(impl_->api, statement.get(), 10);
        item.confidence = parse_double_or_zero(column_string(impl_->api, statement.get(), 11));
        item.resolution_strategy = column_string(impl_->api, statement.get(), 12);
        item.resolution_evidence = column_string(impl_->api, statement.get(), 13);
        item.unresolved = impl_->api.column_int(statement.get(), 14) != 0;
        relationships.push_back(item);
    }
    return relationships;
}

std::vector<SymbolReferenceFact> SqliteDatabase::unresolved_references(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT r.id, COALESCE(r.source_symbol_id, 0), COALESCE(r.target_symbol_id, 0), "
        "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.reference_text), "
        "r.source_file, r.language, r.line, r.column_number, r.reference_text, r.relationship_type, "
        "COALESCE(r.confidence, 0), r.resolution_strategy, COALESCE(r.resolution_evidence, ''), r.is_unresolved "
        "FROM symbol_references r "
        "LEFT JOIN symbols source ON source.id = r.source_symbol_id "
        "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
        "WHERE r.repository_id = ? AND r.is_unresolved = 1 "
        "ORDER BY r.source_file, r.line, r.column_number, r.relationship_type, r.reference_text;"};
    statement.bind_int64(1, repository_id);

    std::vector<SymbolReferenceFact> references;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read unresolved references: "} + impl_->api.errmsg(impl_->database));
        }

        SymbolReferenceFact item;
        item.reference_id = impl_->api.column_int64(statement.get(), 0);
        item.source_symbol_id = impl_->api.column_int64(statement.get(), 1);
        item.target_symbol_id = impl_->api.column_int64(statement.get(), 2);
        item.source_symbol = column_string(impl_->api, statement.get(), 3);
        item.target_symbol = column_string(impl_->api, statement.get(), 4);
        item.source_file = column_string(impl_->api, statement.get(), 5);
        item.language = column_string(impl_->api, statement.get(), 6);
        item.line = impl_->api.column_int(statement.get(), 7);
        item.column = impl_->api.column_int(statement.get(), 8);
        item.reference_text = column_string(impl_->api, statement.get(), 9);
        item.relationship_type = column_string(impl_->api, statement.get(), 10);
        item.confidence = parse_double_or_zero(column_string(impl_->api, statement.get(), 11));
        item.resolution_strategy = column_string(impl_->api, statement.get(), 12);
        item.resolution_evidence = column_string(impl_->api, statement.get(), 13);
        item.unresolved = impl_->api.column_int(statement.get(), 14) != 0;
        references.push_back(item);
    }
    return references;
}
std::optional<FactSymbol> SqliteDatabase::symbol_by_id(long long repository_id, long long symbol_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT s.id, COALESCE(s.parent_symbol_id, 0), COALESCE(s.stable_id, ''), f.relative_path, f.absolute_path, "
        "COALESCE(s.language, f.language, ''), s.kind, s.name, COALESCE(s.qualified_name, ''), "
        "COALESCE(s.signature, ''), COALESCE(s.visibility, ''), COALESCE(s.line_start, 0), COALESCE(s.line_end, 0), "
        "COALESCE(parent.qualified_name, parent.name, '') "
        "FROM symbols s JOIN files f ON f.id = s.file_id "
        "LEFT JOIN symbols parent ON parent.id = s.parent_symbol_id "
        "WHERE s.repository_id = ? AND s.id = ? AND s.is_active = 1 AND f.is_active = 1;"};
    statement.bind_int64(1, repository_id);
    statement.bind_int64(2, symbol_id);

    const int result = impl_->api.step(statement.get());
    if (result == sqlite_done) {
        return std::nullopt;
    }
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read graph symbol: "} + impl_->api.errmsg(impl_->database));
    }

    FactSymbol item;
    item.row_id = impl_->api.column_int64(statement.get(), 0);
    item.parent_row_id = impl_->api.column_int64(statement.get(), 1);
    item.stable_id = column_string(impl_->api, statement.get(), 2);
    item.file_path = column_string(impl_->api, statement.get(), 3);
    item.absolute_path = column_string(impl_->api, statement.get(), 4);
    item.language = column_string(impl_->api, statement.get(), 5);
    item.kind = column_string(impl_->api, statement.get(), 6);
    item.name = column_string(impl_->api, statement.get(), 7);
    item.qualified_name = column_string(impl_->api, statement.get(), 8);
    item.signature = column_string(impl_->api, statement.get(), 9);
    item.visibility = column_string(impl_->api, statement.get(), 10);
    item.line_start = impl_->api.column_int(statement.get(), 11);
    item.line_end = impl_->api.column_int(statement.get(), 12);
    item.parent_scope = column_string(impl_->api, statement.get(), 13);
    if (item.stable_id.empty()) {
        item.stable_id = make_stable_symbol_id(item.file_path, item.language, item.kind, item.qualified_name, item.name, item.signature, item.line_start);
    }
    return item;
}

std::vector<SymbolRelationshipFact> SqliteDatabase::graph_relationships_for_symbol(
    long long repository_id,
    long long symbol_id,
    const std::string& direction,
    double min_confidence)
{
    const bool callers = direction == "callers";
    const bool callees = direction == "callees";
    Statement statement{
        impl_->api,
        impl_->database,
        callers
            ? "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
              "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text), "
              "COALESCE(r.source_file, ''), COALESCE(r.language, ''), COALESCE(r.line, 0), COALESCE(r.column_number, 0), "
              "r.relation_type, COALESCE(r.target_text, r.source_text, ''), COALESCE(r.confidence, 0), "
              "COALESCE(r.resolution_strategy, ''), COALESCE(r.resolution_evidence, ''), COALESCE(r.is_unresolved, 0) "
              "FROM symbol_relations r JOIN symbols source ON source.id = r.source_symbol_id "
              "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
              "WHERE r.repository_id = ? AND r.target_symbol_id = ? AND COALESCE(r.confidence, 0) >= ? "
              "ORDER BY COALESCE(r.confidence, 0) DESC, r.relation_type, r.source_file, r.line, r.column_number, source.qualified_name, target.qualified_name;"
            : callees
                ? "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
                  "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text), "
                  "COALESCE(r.source_file, ''), COALESCE(r.language, ''), COALESCE(r.line, 0), COALESCE(r.column_number, 0), "
                  "r.relation_type, COALESCE(r.target_text, r.source_text, ''), COALESCE(r.confidence, 0), "
                  "COALESCE(r.resolution_strategy, ''), COALESCE(r.resolution_evidence, ''), COALESCE(r.is_unresolved, 0) "
                  "FROM symbol_relations r JOIN symbols source ON source.id = r.source_symbol_id "
                  "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
                  "WHERE r.repository_id = ? AND r.source_symbol_id = ? AND COALESCE(r.confidence, 0) >= ? "
                  "ORDER BY COALESCE(r.confidence, 0) DESC, r.relation_type, r.source_file, r.line, r.column_number, source.qualified_name, target.qualified_name;"
                : "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
                  "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text), "
                  "COALESCE(r.source_file, ''), COALESCE(r.language, ''), COALESCE(r.line, 0), COALESCE(r.column_number, 0), "
                  "r.relation_type, COALESCE(r.target_text, r.source_text, ''), COALESCE(r.confidence, 0), "
                  "COALESCE(r.resolution_strategy, ''), COALESCE(r.resolution_evidence, ''), COALESCE(r.is_unresolved, 0) "
                  "FROM symbol_relations r JOIN symbols source ON source.id = r.source_symbol_id "
                  "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
                  "WHERE r.repository_id = ? AND (r.source_symbol_id = ? OR r.target_symbol_id = ?) AND COALESCE(r.confidence, 0) >= ? "
                  "ORDER BY COALESCE(r.confidence, 0) DESC, r.relation_type, r.source_file, r.line, r.column_number, source.qualified_name, target.qualified_name;"};
    statement.bind_int64(1, repository_id);
    statement.bind_int64(2, symbol_id);
    if (callers || callees) {
        statement.bind_double(3, min_confidence);
    } else {
        statement.bind_int64(3, symbol_id);
        statement.bind_double(4, min_confidence);
    }

    std::vector<SymbolRelationshipFact> relationships;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read graph relationships: "} + impl_->api.errmsg(impl_->database));
        }

        SymbolRelationshipFact item;
        item.relationship_id = impl_->api.column_int64(statement.get(), 0);
        item.source_symbol_id = impl_->api.column_int64(statement.get(), 1);
        item.target_symbol_id = impl_->api.column_int64(statement.get(), 2);
        item.source_symbol = column_string(impl_->api, statement.get(), 3);
        item.target_symbol = column_string(impl_->api, statement.get(), 4);
        item.source_file = column_string(impl_->api, statement.get(), 5);
        item.language = column_string(impl_->api, statement.get(), 6);
        item.line = impl_->api.column_int(statement.get(), 7);
        item.column = impl_->api.column_int(statement.get(), 8);
        item.relationship_type = column_string(impl_->api, statement.get(), 9);
        item.reference_text = column_string(impl_->api, statement.get(), 10);
        item.confidence = parse_double_or_zero(column_string(impl_->api, statement.get(), 11));
        item.resolution_strategy = column_string(impl_->api, statement.get(), 12);
        item.resolution_evidence = column_string(impl_->api, statement.get(), 13);
        item.unresolved = impl_->api.column_int(statement.get(), 14) != 0;
        relationships.push_back(item);
    }
    return relationships;
}
std::vector<ArchitectureEdgeFact> SqliteDatabase::architecture_edges(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT r.id, r.source_symbol_id, COALESCE(r.target_symbol_id, 0), "
        "COALESCE(source.qualified_name, source.name, ''), COALESCE(target.qualified_name, target.name, r.target_text, ''), "
        "COALESCE(source_file.relative_path, r.source_file, ''), COALESCE(target_file.relative_path, ''), "
        "COALESCE(r.relation_type, ''), COALESCE(r.resolution_strategy, ''), COALESCE(r.confidence, 0) "
        "FROM symbol_relations r "
        "JOIN symbols source ON source.id = r.source_symbol_id "
        "JOIN files source_file ON source_file.id = source.file_id "
        "LEFT JOIN symbols target ON target.id = r.target_symbol_id "
        "LEFT JOIN files target_file ON target_file.id = target.file_id "
        "WHERE r.repository_id = ? AND COALESCE(r.is_unresolved, 0) = 0 AND COALESCE(r.target_symbol_id, 0) > 0 "
        "AND source.is_active = 1 AND source_file.is_active = 1 "
        "ORDER BY COALESCE(r.relation_type, ''), COALESCE(source_file.relative_path, r.source_file, ''), "
        "COALESCE(target_file.relative_path, ''), r.line, r.column_number, source.qualified_name, target.qualified_name, r.id;"};
    statement.bind_int64(1, repository_id);

    std::vector<ArchitectureEdgeFact> edges;
    while (true) {
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to read architecture graph: "} + impl_->api.errmsg(impl_->database));
        }

        ArchitectureEdgeFact item;
        item.relationship_id = impl_->api.column_int64(statement.get(), 0);
        item.source_symbol_id = impl_->api.column_int64(statement.get(), 1);
        item.target_symbol_id = impl_->api.column_int64(statement.get(), 2);
        item.source_symbol = column_string(impl_->api, statement.get(), 3);
        item.target_symbol = column_string(impl_->api, statement.get(), 4);
        item.source_file = column_string(impl_->api, statement.get(), 5);
        item.target_file = column_string(impl_->api, statement.get(), 6);
        item.relationship_type = column_string(impl_->api, statement.get(), 7);
        item.resolution_strategy = column_string(impl_->api, statement.get(), 8);
        item.confidence = parse_double_or_zero(column_string(impl_->api, statement.get(), 9));
        edges.push_back(item);
    }
    return edges;
}
long long find_file_id_for_scip(const SqliteApi& api, sqlite3* database, long long repository_id, const std::string& file_path)
{
    if (file_path.empty()) {
        return 0;
    }
    const auto normalized = normalize_fact_path(file_path);
    Statement statement{
        api,
        database,
        "SELECT id, relative_path, absolute_path FROM files WHERE repository_id = ? AND is_active = 1 ORDER BY relative_path;"};
    statement.bind_int64(1, repository_id);
    while (true) {
        const int result = api.step(statement.get());
        if (result == sqlite_done) {
            break;
        }
        if (result != sqlite_row) {
            throw std::runtime_error(std::string{"Failed to map SCIP file: "} + api.errmsg(database));
        }
        const auto relative = column_string(api, statement.get(), 1);
        const auto absolute = column_string(api, statement.get(), 2);
        if (normalize_fact_path(relative) == normalized || normalize_fact_path(absolute) == normalized) {
            return api.column_int64(statement.get(), 0);
        }
    }
    return 0;
}

ScipImportSummary SqliteDatabase::import_scip_facts(
    long long repository_id,
    const std::string& source_path,
    const std::vector<ScipSymbolFact>& symbols,
    const std::vector<ScipReferenceFact>& references)
{
    ScipImportSummary summary;
    summary.source_path = source_path;
    summary.symbols_seen = static_cast<int>(symbols.size());
    summary.references_seen = static_cast<int>(references.size());

    Transaction transaction{impl_->api, impl_->database};

    auto find_symbol = [&](const std::string& symbol_name, const std::string& file_path) {
        if (symbol_name.empty()) {
            return 0LL;
        }
        const auto file_id = find_file_id_for_scip(impl_->api, impl_->database, repository_id, file_path);
        Statement statement{
            impl_->api,
            impl_->database,
            file_id > 0
                ? "SELECT s.id FROM symbols s WHERE s.repository_id = ? AND s.is_active = 1 "
                  "AND (s.qualified_name = ? OR s.name = ?) AND s.file_id = ? "
                  "ORDER BY CASE WHEN s.qualified_name = ? THEN 0 ELSE 1 END, s.line_start, s.id LIMIT 1;"
                : "SELECT s.id FROM symbols s WHERE s.repository_id = ? AND s.is_active = 1 "
                  "AND (s.qualified_name = ? OR s.name = ?) "
                  "ORDER BY CASE WHEN s.qualified_name = ? THEN 0 ELSE 1 END, s.line_start, s.id LIMIT 1;"};
        statement.bind_int64(1, repository_id);
        statement.bind_text(2, symbol_name);
        statement.bind_text(3, symbol_name);
        if (file_id > 0) {
            statement.bind_int64(4, file_id);
            statement.bind_text(5, symbol_name);
        } else {
            statement.bind_text(4, symbol_name);
        }
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_row) {
            return impl_->api.column_int64(statement.get(), 0);
        }
        if (result != sqlite_done) {
            throw std::runtime_error(std::string{"Failed to map SCIP symbol: "} + impl_->api.errmsg(impl_->database));
        }
        return 0LL;
    };

    auto insert_scip_symbol = [&](const ScipSymbolFact& symbol) {
        const long long file_id = find_file_id_for_scip(impl_->api, impl_->database, repository_id, symbol.file_path);
        if (file_id == 0 || symbol.name.empty()) {
            return 0LL;
        }
        Statement statement{
            impl_->api,
            impl_->database,
            "INSERT INTO symbols ("
            "repository_id, file_id, language, kind, name, qualified_name, signature, return_type, visibility, modifiers, "
            "parent_symbol_id, line_start, line_end, char_start, char_end, char_count, stable_id, description, tags, is_active"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, '', '', '', NULL, ?, ?, 0, 0, 0, ?, '', 'scip', 1);"};
        const auto language = symbol.language.empty() ? std::string{"scip"} : symbol.language;
        const auto kind = symbol.kind.empty() ? std::string{"symbol"} : symbol.kind;
        statement.bind_int64(1, repository_id);
        statement.bind_int64(2, file_id);
        statement.bind_text(3, language);
        statement.bind_text(4, kind);
        statement.bind_text(5, symbol.name);
        statement.bind_text(6, symbol.qualified_name);
        statement.bind_text(7, symbol.signature);
        statement.bind_int(8, symbol.line_start);
        statement.bind_int(9, symbol.line_end <= 0 ? symbol.line_start : symbol.line_end);
        statement.bind_text(10, make_stable_symbol_id(symbol.file_path, language, kind, symbol.qualified_name, symbol.name, symbol.signature, symbol.line_start));
        statement.step_done(impl_->database);
        return impl_->api.last_insert_rowid(impl_->database);
    };

    std::unordered_map<std::string, long long> scip_symbol_to_id;
    for (const auto& symbol : symbols) {
        const auto name = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
        long long symbol_id = find_symbol(name, symbol.file_path);
        if (symbol_id != 0) {
            ++summary.symbols_mapped;
        } else {
            symbol_id = insert_scip_symbol(symbol);
            if (symbol_id != 0) {
                ++summary.symbols_inserted;
            }
        }
        if (symbol_id != 0) {
            if (!symbol.scip_symbol.empty()) {
                scip_symbol_to_id[symbol.scip_symbol] = symbol_id;
            }
            if (!symbol.qualified_name.empty()) {
                scip_symbol_to_id[symbol.qualified_name] = symbol_id;
            }
            if (!symbol.name.empty()) {
                scip_symbol_to_id[symbol.name] = symbol_id;
            }
        }
    }

    auto resolve_symbol_text = [&](const std::string& symbol_name, const std::string& file_path) {
        const auto known = scip_symbol_to_id.find(symbol_name);
        if (known != scip_symbol_to_id.end()) {
            return known->second;
        }
        return find_symbol(symbol_name, file_path);
    };

    Statement reference_statement{
        impl_->api,
        impl_->database,
        "INSERT INTO symbol_references ("
        "repository_id, file_id, source_symbol_id, target_symbol_id, source_file, line, column_number, "
        "reference_text, relationship_type, language, confidence, resolution_strategy, resolution_evidence, is_unresolved"
        ") VALUES (?, ?, NULLIF(?, 0), NULLIF(?, 0), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"};
    Statement relation_statement{
        impl_->api,
        impl_->database,
        "INSERT INTO symbol_relations ("
        "repository_id, source_symbol_id, target_symbol_id, relation_type, source_text, target_text, confidence, "
        "source_file, line, column_number, language, resolution_strategy, resolution_evidence, is_unresolved"
        ") VALUES (?, ?, NULLIF(?, 0), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"};

    auto has_conflict = [&](long long source_symbol_id, long long target_symbol_id, const ScipReferenceFact& reference) {
        if (source_symbol_id == 0 || reference.line <= 0) {
            return false;
        }
        Statement statement{
            impl_->api,
            impl_->database,
            "SELECT COUNT(*) FROM symbol_relations WHERE repository_id = ? AND source_symbol_id = ? "
            "AND relation_type = ? AND line = ? AND COALESCE(resolution_strategy, '') <> 'scip' "
            "AND COALESCE(target_symbol_id, 0) <> ?;"};
        statement.bind_int64(1, repository_id);
        statement.bind_int64(2, source_symbol_id);
        statement.bind_text(3, reference.relationship_type.empty() ? std::string{"references"} : reference.relationship_type);
        statement.bind_int(4, reference.line);
        statement.bind_int64(5, target_symbol_id);
        const int result = impl_->api.step(statement.get());
        if (result == sqlite_row) {
            return impl_->api.column_int(statement.get(), 0) > 0;
        }
        if (result != sqlite_done) {
            throw std::runtime_error(std::string{"Failed to count SCIP conflicts: "} + impl_->api.errmsg(impl_->database));
        }
        return false;
    };

    for (const auto& reference : references) {
        const long long file_id = find_file_id_for_scip(impl_->api, impl_->database, repository_id, reference.source_file);
        if (file_id == 0) {
            ++summary.unresolved_references;
            continue;
        }
        const auto relationship_type = reference.relationship_type.empty() ? std::string{"references"} : reference.relationship_type;
        const auto language = reference.language.empty() ? std::string{"scip"} : reference.language;
        const long long source_symbol_id = resolve_symbol_text(reference.source_symbol, reference.source_file);
        const long long target_symbol_id = reference.unresolved ? 0 : resolve_symbol_text(reference.target_symbol, reference.source_file);
        const bool unresolved = reference.unresolved || target_symbol_id == 0;
        const auto reference_text = reference.reference_text.empty() ? reference.target_symbol : reference.reference_text;
        const auto evidence = unresolved
            ? std::string{"SCIP reference imported without mapped target symbol"}
            : std::string{"SCIP exact reference import"};
        const double confidence = unresolved ? 0.6 : 0.995;
        if (has_conflict(source_symbol_id, target_symbol_id, reference)) {
            ++summary.conflicts;
        }

        reference_statement.bind_int64(1, repository_id);
        reference_statement.bind_int64(2, file_id);
        reference_statement.bind_int64(3, source_symbol_id);
        reference_statement.bind_int64(4, target_symbol_id);
        reference_statement.bind_text(5, reference.source_file);
        reference_statement.bind_int(6, reference.line);
        reference_statement.bind_int(7, reference.column);
        reference_statement.bind_text(8, reference_text);
        reference_statement.bind_text(9, relationship_type);
        reference_statement.bind_text(10, language);
        reference_statement.bind_double(11, confidence);
        reference_statement.bind_text(12, "scip");
        reference_statement.bind_text(13, evidence);
        reference_statement.bind_int(14, unresolved ? 1 : 0);
        reference_statement.step_done(impl_->database);
        reference_statement.reset(impl_->database);
        ++summary.references_inserted;
        if (unresolved) {
            ++summary.unresolved_references;
        }

        if (source_symbol_id != 0) {
            relation_statement.bind_int64(1, repository_id);
            relation_statement.bind_int64(2, source_symbol_id);
            relation_statement.bind_int64(3, target_symbol_id);
            relation_statement.bind_text(4, relationship_type);
            relation_statement.bind_text(5, reference.source_symbol);
            relation_statement.bind_text(6, reference.target_symbol.empty() ? reference_text : reference.target_symbol);
            relation_statement.bind_double(7, confidence);
            relation_statement.bind_text(8, reference.source_file);
            relation_statement.bind_int(9, reference.line);
            relation_statement.bind_int(10, reference.column);
            relation_statement.bind_text(11, language);
            relation_statement.bind_text(12, "scip");
            relation_statement.bind_text(13, evidence);
            relation_statement.bind_int(14, unresolved ? 1 : 0);
            relation_statement.step_done(impl_->database);
            relation_statement.reset(impl_->database);
            ++summary.relationships_inserted;
        }
    }

    Statement import_statement{
        impl_->api,
        impl_->database,
        "INSERT INTO scip_imports (repository_id, source_path, symbols_seen, symbols_inserted, symbols_mapped, "
        "references_seen, references_inserted, relationships_inserted, unresolved_references, conflicts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"};
    import_statement.bind_int64(1, repository_id);
    import_statement.bind_text(2, source_path);
    import_statement.bind_int(3, summary.symbols_seen);
    import_statement.bind_int(4, summary.symbols_inserted);
    import_statement.bind_int(5, summary.symbols_mapped);
    import_statement.bind_int(6, summary.references_seen);
    import_statement.bind_int(7, summary.references_inserted);
    import_statement.bind_int(8, summary.relationships_inserted);
    import_statement.bind_int(9, summary.unresolved_references);
    import_statement.bind_int(10, summary.conflicts);
    import_statement.step_done(impl_->database);

    summary.imported_at = "now";
    transaction.commit();
    return summary;
}

std::optional<ScipImportSummary> SqliteDatabase::last_scip_import(long long repository_id)
{
    Statement statement{
        impl_->api,
        impl_->database,
        "SELECT source_path, imported_at, symbols_seen, symbols_inserted, symbols_mapped, references_seen, "
        "references_inserted, relationships_inserted, unresolved_references, conflicts "
        "FROM scip_imports WHERE repository_id = ? ORDER BY imported_at DESC, id DESC LIMIT 1;"};
    statement.bind_int64(1, repository_id);
    const int result = impl_->api.step(statement.get());
    if (result == sqlite_done) {
        return std::nullopt;
    }
    if (result != sqlite_row) {
        throw std::runtime_error(std::string{"Failed to read SCIP import status: "} + impl_->api.errmsg(impl_->database));
    }
    ScipImportSummary summary;
    summary.source_path = column_string(impl_->api, statement.get(), 0);
    summary.imported_at = column_string(impl_->api, statement.get(), 1);
    summary.symbols_seen = impl_->api.column_int(statement.get(), 2);
    summary.symbols_inserted = impl_->api.column_int(statement.get(), 3);
    summary.symbols_mapped = impl_->api.column_int(statement.get(), 4);
    summary.references_seen = impl_->api.column_int(statement.get(), 5);
    summary.references_inserted = impl_->api.column_int(statement.get(), 6);
    summary.relationships_inserted = impl_->api.column_int(statement.get(), 7);
    summary.unresolved_references = impl_->api.column_int(statement.get(), 8);
    summary.conflicts = impl_->api.column_int(statement.get(), 9);
    return summary;
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





