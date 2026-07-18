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
    std::string similarity_signature;
};

struct FolderFingerprint {
    std::string folder_path;
    std::string fingerprint;
    int file_count = 0;
    long long total_size = 0;
};

struct StoredFile {
    long long id = 0;
    std::string relative_path;
    std::string content_hash;
    std::string similarity_signature;
    bool is_active = false;
};

struct SimilarityFile {
    std::string relative_path;
    std::string similarity_signature;
    std::string language;
    long long size_bytes = 0;
};

struct IndexedFileFact {
    long long file_id = 0;
    std::string relative_path;
    std::string absolute_path;
    std::string language;
    long long size_bytes = 0;
    long long line_count = 0;
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

struct VirtualFileInput {
    std::string signal_type;
    std::string virtual_path;
    std::string source_path;
    std::string content_hash;
    std::string content;
    long long size_bytes = 0;
    long long line_count = 0;
    bool truncated = false;
};

struct VirtualFileFact {
    long long id = 0;
    std::string signal_type;
    std::string source_path;
    std::string virtual_path;
    std::string imported_at;
    std::string content_hash;
    long long size_bytes = 0;
    long long line_count = 0;
    bool truncated = false;
};

struct VirtualFileSearchResult {
    VirtualFileFact file;
    int line = 0;
    std::string snippet;
};

struct FactSymbol {
    long long row_id = 0;
    long long parent_row_id = 0;
    std::string stable_id;
    std::string file_path;
    std::string absolute_path;
    std::string language;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string visibility;
    int line_start = 0;
    int line_end = 0;
    std::string parent_scope;
};

struct SourceSnippet {
    std::string file_path;
    std::string absolute_path;
    std::string language;
    int line_start = 0;
    int line_end = 0;
    std::string code;
};

struct ContextDescription {
    long long id = 0;
    std::string target_type;
    long long target_id = 0;
    std::string target_key;
    std::string description;
    std::string source;
    std::string updated_at;
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

struct SymbolReferenceFact {
    long long reference_id = 0;
    long long source_symbol_id = 0;
    long long target_symbol_id = 0;
    std::string source_symbol;
    std::string target_symbol;
    std::string source_file;
    std::string language;
    int line = 0;
    int column = 0;
    std::string reference_text;
    std::string relationship_type;
    double confidence = 0.0;
    std::string resolution_strategy;
    std::string resolution_evidence;
    bool unresolved = false;
};

struct SymbolRelationshipFact {
    long long relationship_id = 0;
    long long source_symbol_id = 0;
    long long target_symbol_id = 0;
    std::string source_symbol;
    std::string target_symbol;
    std::string source_file;
    std::string language;
    int line = 0;
    int column = 0;
    std::string relationship_type;
    std::string reference_text;
    double confidence = 0.0;
    std::string resolution_strategy;
    std::string resolution_evidence;
    bool unresolved = false;
};
struct ArchitectureEdgeFact {
    long long relationship_id = 0;
    long long source_symbol_id = 0;
    long long target_symbol_id = 0;
    std::string source_symbol;
    std::string target_symbol;
    std::string source_file;
    std::string target_file;
    std::string relationship_type;
    std::string resolution_strategy;
    double confidence = 0.0;
};
struct ScipSymbolFact {
    std::string scip_symbol;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string file_path;
    std::string language;
    int line_start = 0;
    int line_end = 0;
};

struct ScipReferenceFact {
    std::string source_symbol;
    std::string target_symbol;
    std::string source_file;
    std::string language;
    int line = 0;
    int column = 0;
    std::string reference_text;
    std::string relationship_type;
    bool unresolved = false;
};

struct ScipImportSummary {
    int symbols_seen = 0;
    int symbols_inserted = 0;
    int symbols_mapped = 0;
    int references_seen = 0;
    int references_inserted = 0;
    int relationships_inserted = 0;
    int unresolved_references = 0;
    int conflicts = 0;
    std::string source_path;
    std::string imported_at;
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
    long long symbol_references = 0;
    long long context_descriptions = 0;
    long long virtual_files = 0;
    long long scip_imports = 0;
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
    std::unordered_map<std::string, FolderFingerprint> read_folder_fingerprints(long long repository_id);
    void replace_folder_fingerprints(long long repository_id, const std::vector<FolderFingerprint>& folders);
    std::string repository_fingerprint(long long repository_id);
    void update_repository_fingerprint(long long repository_id, const std::string& fingerprint);
    std::vector<SimilarityFile> read_similarity_files(long long repository_id);
    std::vector<IndexedFileFact> active_files(long long repository_id);
    long long create_snapshot(long long repository_id);
    long long upsert_file(long long repository_id, const FileMetadata& file, long long snapshot_id, bool is_new_file);
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
    ParseSaveStats save_parse_result(long long repository_id, long long file_id, const ParseResult& result, bool lite_mode = false);
    int mark_symbols_inactive_for_file(long long file_id);
    int count_active_symbols(long long repository_id);
    DatabaseRowCounts count_rows();
    void prune_lite_metadata();
    void compact();
    std::vector<SearchResult> search(long long repository_id, const SearchOptions& options);
    VirtualFileFact upsert_virtual_file(long long repository_id, const VirtualFileInput& input);
    std::vector<VirtualFileFact> list_virtual_files(long long repository_id);
    std::vector<VirtualFileSearchResult> search_virtual_files(long long repository_id, const std::string& query, int limit = 20);
    bool delete_virtual_file(long long repository_id, const std::string& virtual_path_or_source_path);
    std::vector<FactSymbol> resolve_symbols(
        long long repository_id,
        const std::string& symbol_name,
        const std::optional<std::string>& file_path = std::nullopt);
    std::vector<FactSymbol> symbols_for_file(long long repository_id, const std::string& file_path);
    SourceSnippet read_snippet(long long repository_id, const std::string& file_path, int line_start, int line_end);
    void upsert_context_description(long long repository_id, const ContextDescription& description);
    std::optional<ContextDescription> context_description_for(
        long long repository_id,
        const std::string& target_type,
        long long target_id,
        const std::string& target_key = "");
    std::vector<ContextDescription> context_descriptions(long long repository_id);
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
    std::vector<SymbolReferenceFact> references_for_symbol(long long repository_id, const std::string& symbol_name);
    std::vector<SymbolRelationshipFact> relationships_for_symbol(
        long long repository_id,
        const std::string& symbol_name,
        const std::optional<std::string>& relationship_type = std::nullopt);
    std::vector<SymbolReferenceFact> unresolved_references(long long repository_id);
    std::optional<FactSymbol> symbol_by_id(long long repository_id, long long symbol_id);
    ScipImportSummary import_scip_facts(
        long long repository_id,
        const std::string& source_path,
        const std::vector<ScipSymbolFact>& symbols,
        const std::vector<ScipReferenceFact>& references);
    std::optional<ScipImportSummary> last_scip_import(long long repository_id);
    std::vector<SymbolRelationshipFact> graph_relationships_for_symbol(
        long long repository_id,
        long long symbol_id,
        const std::string& direction,
        double min_confidence);
    std::vector<ArchitectureEdgeFact> architecture_edges(long long repository_id);
    void update_symbol_enrichment(long long symbol_id, const EnrichmentResult& result);

private:
    void exec(const std::string& sql);

    struct Impl;
    Impl* impl_;
};

} // namespace repolens




