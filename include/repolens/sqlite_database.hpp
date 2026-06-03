#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace repolens {

struct ParseResult;

struct RepositoryStatus {
    long long repository_id = 0;
    std::string repo_root;
    std::string index_root;
    int schema_version = 0;
    std::string last_indexed_at;
};

struct FileMetadata {
    std::string relative_path;
    std::string absolute_path;
    std::string extension;
    long long size_bytes = 0;
    long long line_count = 0;
    long long char_count = 0;
    std::string last_modified_time;
    std::string content_hash;
};

struct StoredFile {
    long long id = 0;
    std::string relative_path;
    std::string content_hash;
    bool is_active = false;
};

struct SearchOptions {
    std::string query;
    std::string kind;
    int limit = 20;
};

struct SearchResult {
    std::string result_type;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string file_path;
    int line_start = 0;
    int line_end = 0;
};

struct ContextSymbolCandidate {
    long long symbol_id = 0;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string relative_path;
    std::string absolute_path;
    int line_start = 0;
    int line_end = 0;
};

struct ContextRelatedSymbol {
    ContextSymbolCandidate symbol;
    std::string relation_type;
    std::string source_qualified_name;
};

struct SymbolForEnrichment {
    long long symbol_id = 0;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string file_path;
    int line_start = 0;
    int line_end = 0;
};

struct EnrichmentResult {
    std::string description;
    std::string tags;
    std::string ai_description;
    std::string ai_tags;
    std::string ai_model;
};

struct ParseSaveStats {
    int symbols_inserted = 0;
    int symbols_deleted = 0;
    int symbols_deactivated = 0;
};

struct DatabaseRowCounts {
    long long repositories = 0;
    long long files = 0;
    long long symbols = 0;
    long long symbol_parameters = 0;
    long long symbol_relations = 0;
    long long snapshots = 0;
    long long changes = 0;
};

class SqliteDatabase {
public:
    explicit SqliteDatabase(const std::filesystem::path& database_path);
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    void create_schema();
    void insert_repository(
        const std::filesystem::path& repo_root,
        const std::filesystem::path& index_root,
        int schema_version);
    std::optional<RepositoryStatus> read_repository_status();
    std::unordered_map<std::string, StoredFile> read_files(long long repository_id);
    long long create_snapshot(long long repository_id);
    void upsert_file(long long repository_id, const FileMetadata& file, long long snapshot_id, bool is_new_file);
    void mark_file_deleted(long long file_id, long long snapshot_id);
    void record_change(
        long long repository_id,
        long long snapshot_id,
        const std::string& entity_type,
        long long entity_id,
        const std::string& change_type,
        const std::string& old_hash,
        const std::string& new_hash,
        const std::string& old_path,
        const std::string& new_path);
    void update_last_indexed_at(long long repository_id);
    ParseSaveStats save_parse_result(long long repository_id, long long file_id, const ParseResult& result);
    int mark_symbols_inactive_for_file(long long file_id);
    int count_active_symbols(long long repository_id);
    DatabaseRowCounts count_rows();
    std::vector<SearchResult> search(long long repository_id, const SearchOptions& options);
    std::vector<ContextSymbolCandidate> find_context_symbols(
        long long repository_id,
        const std::string& symbol_name,
        bool partial_match = false);
    std::vector<ContextSymbolCandidate> active_context_symbols(long long repository_id);
    std::vector<ContextRelatedSymbol> find_related_type_symbols(
        long long repository_id,
        const std::vector<long long>& source_symbol_ids);
    std::vector<std::string> active_file_paths(long long repository_id);
    std::vector<SymbolForEnrichment> symbols_for_enrichment(long long repository_id, bool changed_only);
    void update_symbol_enrichment(long long symbol_id, const EnrichmentResult& result);

private:
    void exec(const std::string& sql);

    struct Impl;
    Impl* impl_;
};

} // namespace repolens
