#include "repolens/file_scanner.hpp"
#include "repolens/interpreters/apple_interpreter.hpp"
#include "repolens/interpreters/build_file_interpreter.hpp"
#include "repolens/interpreters/cpp_interpreter.hpp"
#include "repolens/interpreters/csharp_interpreter.hpp"
#include "repolens/interpreters/devops_interpreter.hpp"
#include "repolens/interpreters/go_interpreter.hpp"
#include "repolens/interpreters/interpreter_registry.hpp"
#include "repolens/interpreters/matlab_interpreter.hpp"
#include "repolens/interpreters/jvm_interpreter.hpp"
#include "repolens/interpreters/php_interpreter.hpp"
#include "repolens/interpreters/python_interpreter.hpp"
#include "repolens/interpreters/r_interpreter.hpp"
#include "repolens/interpreters/rust_interpreter.hpp"
#include "repolens/interpreters/ruby_interpreter.hpp"
#include "repolens/interpreters/shell_interpreter.hpp"
#include "repolens/interpreters/sql_interpreter.hpp"
#include "repolens/interpreters/web_interpreter.hpp"
#include "repolens/interpreters/xml_interpreter.hpp"
#include "repolens/version.hpp"
#include "repolens/sqlite_database.hpp"

#include <exception>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr int schema_version = 1;

void print_help()
{
    std::cout
        << "RepoLens " << repolens::version << '\n'
        << '\n'
        << "Usage: repolens <command> [options]\n"
        << '\n'
        << "Commands:\n"
        << "  init <repo_path> --index-dir <index_path>    Initialize an external index.\n"
        << "  status --index-dir <index_path> [--show-scip] [--fingerprints] [--staged] [--similarity] Show index status.\n"
        << "  update --index-dir <index_path> [--format text|json] [--quiet] [--verbose] [--no-progress] [--lite] [--show-diff] [--optimize-large-repo] [--no-similarity-prioritization] [--scip-index <path>]\n"
        << "                                               Scan files and update the index.\n"
        << "  update --index-dir <index_path> --staged [--lite]\n"
        << "                                               Build replacement databases in a temp path and promote atomically.\n"
        << "  updateroot [--include-file <path>] [--exclude-file <path>] [--lite] [--staged]\n"
        << "                                               Update repolens.db beside the executable using path lists.\n"
        << "  update-files --index-dir <index_path> --repo-root <repo_path> --files <a,b> [--lite] [--replace]\n"
        << "                                               Parse only listed files into the index.\n"
        << "  diagnostics --index-dir <index_path>         Show SQLite row counts and database size.\n"
        << "  compact --index-dir <index_path>             Reclaim unused SQLite pages and optimize the database.\n"
        << "  import-scip --index-dir <index_path> <path>  Import optional SCIP JSON facts into the index.\n"
        << "  search --index-dir <index_path> --query <text> [--kind <kind>] [--limit <n>] [--partial] [--format text|json]\n"
        << "  resolve-symbol --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]\n"
        << "  snippet --index-dir <index_path> --file <path> --start <n> --end <n> [--format text|json|--json]\n"
        << "  symbol-range --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]\n"
        << "  compact-view --index-dir <index_path> [--file <path>|--symbol <name>] [--max-depth <n>] [--budget-chars <n>] [--include-private] [--format text|json|--json]\n"
        << "  describe --index-dir <index_path> [--file <path>|--symbol <name>|--all] [--deterministic] [--refresh] [--format text|json|--json]\n"
        << "  refs --index-dir <index_path> <symbol> [--format text|json|--json]\n"
        << "  relationships --index-dir <index_path> <symbol> [--type <kind>] [--format text|json|--json]\n"
        << "  unresolved-refs --index-dir <index_path> [--format text|json|--json]\n"
        << "  trace --index-dir <index_path> <symbol> [--direction callees|callers|both] [--depth <n>] [--min-confidence <n>] [--max-results <n>] [--budget-chars <n>] [--format text|json|--json]\n"
        << "  architecture --index-dir <index_path> [--communities|--hubs] [--level file|symbol] [--top <n>] [--seed <id-or-name>] [--format text|json|--json]\n"
        << "  impact --index-dir <index_path> [<symbol>|--symbol <symbol>|--file <path>] [--depth <n>] [--max-results <n>] [--budget-chars <n>] [--include-paths] [--format text|json|--json]\n"
        << "  quality --index-dir <index_path> [--dead-code] [--complexity] [--unresolved] [--max-function-lines <n>] [--max-file-lines <n>] [--complexity-threshold <n>] [--top <n>] [--format text|json|--json]\n"
        << "  signals import --index-dir <index_path> --type <type> --file <path> [--max-bytes <n>] [--format text|json|--json]\n"
        << "  signals list|search|delete --index-dir <index_path> [...] [--format text|json|--json]\n"
        << "  context --index-dir <index_path> [<symbol>|--symbols <A,B>] [--signals <query>] [--situated] [--partial] [--basic] [--level <n>] [--budget-chars <n>] [--include-tree] [--include-types] [--grow --grow-files <a,b>] --format json\n"
        << "  direct-context --file <path> --signature <text> [--repo-root <path>] [--budget-chars <n>] --format json\n"
        << "                                               Parse one file in real time without touching a database.\n"
        << "  serve --index-dir <index_path> [--port 7123] Start local HTTP API on 127.0.0.1.\n"
        << "  serve --index-dir <index_path> --mcp         Start MCP JSON-RPC server over stdio.\n"
        << "  enrich --index-dir <index_path> [--changed-only] Enrich symbols with optional AI metadata.\n"
        << '\n'
        << "Global options:\n"
        << "  --help                                      Show this help message.\n"
        << "  --version                                   Show the RepoLens version.\n";
}

void print_version()
{
    std::cout << "RepoLens " << repolens::version << '\n';
}

std::optional<std::filesystem::path> read_option_path(int argc, char* argv[], std::string_view option)
{
    for (int index = 1; index < argc - 1; ++index) {
        if (std::string_view{argv[index]} == option) {
            return std::filesystem::path{argv[index + 1]};
        }
    }

    return std::nullopt;
}

std::optional<std::string> read_option_string(int argc, char* argv[], std::string_view option)
{
    for (int index = 1; index < argc - 1; ++index) {
        if (std::string_view{argv[index]} == option) {
            return std::string{argv[index + 1]};
        }
    }

    return std::nullopt;
}

int read_option_int(int argc, char* argv[], std::string_view option, int fallback)
{
    const auto value = read_option_string(argc, argv, option);
    if (!value) {
        return fallback;
    }

    return std::stoi(*value);
}

std::filesystem::path canonical_existing_directory(const std::filesystem::path& path, std::string_view name)
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error(std::string{name} + " does not exist: " + path.string());
    }

    if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error(std::string{name} + " is not a directory: " + path.string());
    }

    return std::filesystem::canonical(path);
}


std::filesystem::path canonical_existing_file(const std::filesystem::path& path, std::string_view name)
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error(std::string{name} + " does not exist: " + path.string());
    }

    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(std::string{name} + " is not a file: " + path.string());
    }

    return std::filesystem::canonical(path);
}
std::filesystem::path prepare_index_directory(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path);

    if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error("index_dir is not a directory: " + path.string());
    }

    return std::filesystem::canonical(path);
}

std::string comparable_path_text(const std::filesystem::path& path)
{
    auto text = path.lexically_normal().string();
    for (char& character : text) {
        if (character == '/') {
            character = '\\';
        }
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

bool is_same_or_child_path(const std::filesystem::path& parent, const std::filesystem::path& child)
{
    const auto parent_text = comparable_path_text(parent);
    const auto child_text = comparable_path_text(child);

    if (child_text == parent_text) {
        return true;
    }

    if (child_text.size() <= parent_text.size()) {
        return false;
    }

    return child_text.rfind(parent_text, 0) == 0 && child_text[parent_text.size()] == '\\';
}

void validate_external_index(const std::filesystem::path& repo_root, const std::filesystem::path& index_root)
{
    if (is_same_or_child_path(repo_root, index_root)) {
        throw std::runtime_error("index_dir must be outside repo_path.");
    }
}

struct ParseCounts {
    int parsed = 0;
    int skipped = 0;
    int failed = 0;
};

struct SimilarityGroupSummary {
    int id = 0;
    std::string representative_signature;
    std::vector<std::string> files;
};

struct UpdateSummary {
    std::string repo_root;
    std::string index_root;
    std::string database_path;
    std::string started_at;
    std::string finished_at;
    double elapsed_seconds = 0.0;
    int discovered = 0;
    int ignored = 0;
    int processed = 0;
    int added = 0;
    int modified = 0;
    int deleted = 0;
    int unchanged = 0;
    int files_scanned = 0;
    int files_reindexed = 0;
    int folders_changed = 0;
    int folders_tracked = 0;
    bool similarity_prioritization_enabled = false;
    int similarity_groups = 0;
    int largest_similarity_group = 0;
    std::vector<SimilarityGroupSummary> similarity_group_summaries;
    int failed = 0;
    long long source_bytes = 0;
    int symbols_before = 0;
    int symbols_after = 0;
    int symbols_added = 0;
    int symbols_updated = 0;
    int symbols_deleted = 0;
    int symbols_deactivated = 0;
    long long database_size_before = 0;
    long long database_size_after = 0;
    long long snapshot_id = 0;
    bool lite_mode = false;
    bool show_diff = false;
    std::string repo_fingerprint_before;
    std::string repo_fingerprint_after;
    std::vector<std::string> reindexed_files;
    std::vector<std::string> deleted_files;
    std::vector<std::string> changed_folders;
    ParseCounts parse_counts;
    std::vector<std::string> warnings;
};

std::string format_timestamp(const std::chrono::system_clock::time_point& value)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(value);
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif

    std::ostringstream output;
    output << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

long long file_size_or_zero(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return 0;
    }
    return static_cast<long long>(size);
}

std::string format_bytes(long long bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit_index = 0;
    while (value >= 1024.0 && unit_index < 3) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream output;
    if (unit_index == 0) {
        output << bytes << ' ' << units[unit_index];
    } else {
        output << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2) << value << ' ' << units[unit_index];
    }
    return output.str();
}

std::string fnv1a_hex(const std::string& text)
{
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset_basis;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= prime;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string parent_folder_path(const std::string& relative_path)
{
    const auto slash = relative_path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return relative_path.substr(0, slash);
}

std::vector<std::string> folder_ancestors_for_file(const std::string& relative_path)
{
    std::vector<std::string> folders{"."};
    const auto folder = parent_folder_path(relative_path);
    if (folder == ".") {
        return folders;
    }
    std::string current;
    std::stringstream stream{folder};
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty()) {
            continue;
        }
        if (!current.empty()) {
            current += '/';
        }
        current += segment;
        folders.push_back(current);
    }
    return folders;
}

std::uint64_t parse_hex64(const std::string& text)
{
    std::uint64_t value = 0;
    std::stringstream stream{text};
    stream >> std::hex >> value;
    return value;
}

int hamming_distance64(const std::string& left, const std::string& right)
{
    auto value = parse_hex64(left) ^ parse_hex64(right);
    int count = 0;
    while (value != 0) {
        value &= (value - 1);
        ++count;
    }
    return count;
}

std::string file_similarity_extension(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    auto extension = path.substr(dot);
    for (char& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
}

std::vector<SimilarityGroupSummary> cluster_similarity_records(const std::vector<std::pair<std::string, std::string>>& records)
{
    constexpr int similarity_threshold = 20;
    struct MutableGroup {
        std::string representative_signature;
        std::string extension;
        std::vector<std::string> files;
    };

    std::vector<std::pair<std::string, std::string>> ordered = records;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second < right.second;
        }
        return left.first < right.first;
    });

    std::vector<MutableGroup> groups;
    for (const auto& [path, signature] : ordered) {
        const auto extension = file_similarity_extension(path);
        int best_group = -1;
        int best_distance = 65;
        if (!signature.empty()) {
            for (int index = 0; index < static_cast<int>(groups.size()); ++index) {
                if (groups[index].extension != extension || groups[index].representative_signature.empty()) {
                    continue;
                }
                const int distance = hamming_distance64(signature, groups[index].representative_signature);
                if (distance < best_distance || (distance == best_distance && groups[index].files.front() < groups[best_group].files.front())) {
                    best_distance = distance;
                    best_group = index;
                }
            }
        }
        if (best_group >= 0 && best_distance <= similarity_threshold) {
            groups[best_group].files.push_back(path);
        } else {
            MutableGroup group;
            group.representative_signature = signature;
            group.extension = extension;
            group.files.push_back(path);
            groups.push_back(group);
        }
    }

    std::vector<SimilarityGroupSummary> summaries;
    for (auto& group : groups) {
        std::sort(group.files.begin(), group.files.end());
        SimilarityGroupSummary summary;
        summary.representative_signature = group.representative_signature;
        summary.files = group.files;
        summaries.push_back(std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(), [](const auto& left, const auto& right) {
        if (left.files.size() != right.files.size()) {
            return left.files.size() > right.files.size();
        }
        if (left.representative_signature != right.representative_signature) {
            return left.representative_signature < right.representative_signature;
        }
        return left.files.front() < right.files.front();
    });
    for (int index = 0; index < static_cast<int>(summaries.size()); ++index) {
        summaries[index].id = index + 1;
    }
    return summaries;
}

std::vector<SimilarityGroupSummary> cluster_scanned_files(const std::vector<repolens::FileMetadata>& files)
{
    std::vector<std::pair<std::string, std::string>> records;
    for (const auto& file : files) {
        records.emplace_back(file.relative_path, file.similarity_signature);
    }
    return cluster_similarity_records(records);
}

std::vector<repolens::FileMetadata> order_files_by_similarity(
    const std::vector<repolens::FileMetadata>& files,
    const std::vector<SimilarityGroupSummary>& groups)
{
    std::unordered_map<std::string, repolens::FileMetadata> by_path;
    for (const auto& file : files) {
        by_path.emplace(file.relative_path, file);
    }
    std::vector<repolens::FileMetadata> ordered;
    for (const auto& group : groups) {
        for (const auto& path : group.files) {
            const auto found = by_path.find(path);
            if (found != by_path.end()) {
                ordered.push_back(found->second);
            }
        }
    }
    return ordered;
}
std::vector<repolens::FolderFingerprint> compute_folder_fingerprints(const std::vector<repolens::FileMetadata>& files)
{
    struct FolderAccumulator {
        int file_count = 0;
        long long total_size = 0;
        std::vector<std::string> child_records;
    };
    std::map<std::string, FolderAccumulator> folders;
    for (const auto& file : files) {
        for (const auto& folder : folder_ancestors_for_file(file.relative_path)) {
            auto& accumulator = folders[folder];
            ++accumulator.file_count;
            accumulator.total_size += file.size_bytes;
            accumulator.child_records.push_back(file.relative_path + "|" + file.content_hash + "|" + std::to_string(file.size_bytes) + "|" + file.last_modified_time);
        }
    }

    std::vector<repolens::FolderFingerprint> result;
    for (auto& [folder_path, accumulator] : folders) {
        std::sort(accumulator.child_records.begin(), accumulator.child_records.end());
        std::ostringstream material;
        material << "folder|" << folder_path << '|';
        for (const auto& record : accumulator.child_records) {
            material << record << '\n';
        }
        repolens::FolderFingerprint fingerprint;
        fingerprint.folder_path = folder_path;
        fingerprint.fingerprint = fnv1a_hex(material.str());
        fingerprint.file_count = accumulator.file_count;
        fingerprint.total_size = accumulator.total_size;
        result.push_back(fingerprint);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.folder_path < right.folder_path;
    });
    return result;
}

std::string compute_repository_fingerprint(const std::vector<repolens::FileMetadata>& files, const std::vector<repolens::FolderFingerprint>& folders)
{
    std::ostringstream material;
    material << "repolens-index-v1\n";
    for (const auto& file : files) {
        material << "file|" << file.relative_path << '|' << file.content_hash << '|' << file.size_bytes << '|' << file.last_modified_time << '\n';
    }
    for (const auto& folder : folders) {
        material << "folder|" << folder.folder_path << '|' << folder.fingerprint << '|' << folder.file_count << '|' << folder.total_size << '\n';
    }
    return fnv1a_hex(material.str());
}

std::vector<std::string> changed_folder_paths(
    const std::unordered_map<std::string, repolens::FolderFingerprint>& previous,
    const std::vector<repolens::FolderFingerprint>& current)
{
    std::vector<std::string> changed;
    std::set<std::string> seen;
    for (const auto& folder : current) {
        seen.insert(folder.folder_path);
        const auto existing = previous.find(folder.folder_path);
        if (existing == previous.end() || existing->second.fingerprint != folder.fingerprint) {
            changed.push_back(folder.folder_path);
        }
    }
    for (const auto& [folder_path, fingerprint] : previous) {
        (void)fingerprint;
        if (seen.find(folder_path) == seen.end()) {
            changed.push_back(folder_path);
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    return changed;
}
std::string progress_bar(int processed, int total)
{
    constexpr int width = 40;
    const int clamped_total = std::max(total, 0);
    const int clamped_processed = std::max(0, std::min(processed, clamped_total));
    const int filled = clamped_total == 0 ? width : (clamped_processed * width) / clamped_total;

    std::string bar = "[";
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '-');
    bar += "]";
    return bar;
}

class UpdateProgressReporter {
public:
    virtual ~UpdateProgressReporter() = default;
    virtual void on_scan_started(const UpdateSummary&) {}
    virtual void on_scan_completed(const UpdateSummary&) {}
    virtual void on_file_started(const repolens::FileMetadata&, int, int, const UpdateSummary&) {}
    virtual void on_file_completed(const repolens::FileMetadata&, int, int, const UpdateSummary&) {}
    virtual void on_file_failed(const repolens::FileMetadata&, const std::string&, int, int, const UpdateSummary&) {}
    virtual void on_update_completed(const UpdateSummary&) {}
};

class NullProgressReporter final : public UpdateProgressReporter {
};

class ConsoleProgressReporter final : public UpdateProgressReporter {
public:
    explicit ConsoleProgressReporter(bool verbose)
        : verbose_(verbose)
    {
    }

    void on_scan_started(const UpdateSummary& summary) override
    {
        std::cout
            << "RepoLens Update\n"
            << "---------------\n"
            << "Repo root:   " << summary.repo_root << '\n'
            << "Index dir:   " << summary.index_root << '\n'
            << "Database:    " << summary.database_path << '\n'
            << "Started at:  " << summary.started_at << "\n\n"
            << "Scanning repository...\n";
    }

    void on_scan_completed(const UpdateSummary& summary) override
    {
        std::cout
            << "Files discovered: " << summary.discovered << '\n'
            << "Files ignored:    " << summary.ignored << '\n'
            << "Files to process: " << (summary.discovered - summary.ignored) << "\n\n"
            << "Processing files:\n";
    }

    void on_file_started(const repolens::FileMetadata& file, int processed, int total, const UpdateSummary& summary) override
    {
        print_progress(file, processed - 1, total, summary);
    }

    void on_file_completed(const repolens::FileMetadata& file, int processed, int total, const UpdateSummary& summary) override
    {
        print_progress(file, processed, total, summary);
        if (verbose_) {
            std::cout << "Completed: " << file.relative_path << "\n\n";
        }
    }

    void on_file_failed(const repolens::FileMetadata& file, const std::string& error, int processed, int total, const UpdateSummary& summary) override
    {
        print_progress(file, processed, total, summary);
        std::cout << "Failed: " << file.relative_path << " (" << error << ")\n\n";
    }

    void on_update_completed(const UpdateSummary& summary) override
    {
        std::cout
            << "\nRepoLens Update Completed\n"
            << "-------------------------\n"
            << "Repo root:        " << summary.repo_root << '\n'
            << "Index dir:        " << summary.index_root << '\n'
            << "Database:         " << summary.database_path << '\n'
            << "Mode:             " << (summary.lite_mode ? "lite" : "full") << "\n\n"
            << "Started at:       " << summary.started_at << '\n'
            << "Finished at:      " << summary.finished_at << '\n'
            << "Elapsed time:     " << std::fixed << std::setprecision(2) << summary.elapsed_seconds << " seconds\n\n"
            << "Files:\n"
            << "Discovered:       " << summary.discovered << '\n'
            << "Ignored:          " << summary.ignored << '\n'
            << "Processed:        " << summary.processed << '\n'
            << "Files scanned:    " << summary.files_scanned << '\n'
            << "Files re-indexed: " << summary.files_reindexed << '\n'
            << "Source bytes:     " << summary.source_bytes << '\n'
            << "Added:            " << summary.added << '\n'
            << "Modified:         " << summary.modified << '\n'
            << "Deleted:          " << summary.deleted << '\n'
            << "Unchanged:        " << summary.unchanged << '\n'
            << "Failed:           " << summary.failed << "\n"
            << "Folders tracked:  " << summary.folders_tracked << '\n'
            << "Folders changed:  " << summary.folders_changed << '\n'
            << "Similarity order: " << (summary.similarity_prioritization_enabled ? "enabled" : "disabled") << '\n'
            << "Similarity groups: " << summary.similarity_groups << '\n'
            << "Largest group:    " << summary.largest_similarity_group << "\n\n"
            << "Symbols:\n"
            << "Before:           " << summary.symbols_before << '\n'
            << "After:            " << summary.symbols_after << '\n'
            << "Added:            " << summary.symbols_added << '\n'
            << "Updated:          " << summary.symbols_updated << '\n'
            << "Deleted:          " << summary.symbols_deleted << '\n'
            << "Deactivated:      " << summary.symbols_deactivated << '\n'
            << "Parsed files:     " << summary.parse_counts.parsed << '\n'
            << "Skipped files:    " << summary.parse_counts.skipped << '\n'
            << "Parse failed:     " << summary.parse_counts.failed << "\n\n"
            << "Database:\n"
            << "Path:             " << summary.database_path << '\n'
            << "Size before:      " << format_bytes(summary.database_size_before) << '\n'
            << "Size after:       " << format_bytes(summary.database_size_after) << '\n'
            << "Growth:           " << format_bytes(summary.database_size_after - summary.database_size_before) << "\n\n"
            << "Snapshot:\n"
            << "Snapshot ID:      " << summary.snapshot_id << '\n'
            << "Fingerprint old:  " << summary.repo_fingerprint_before << '\n'
            << "Fingerprint new:  " << summary.repo_fingerprint_after << '\n'
            << "Created at:       " << summary.finished_at << "\n\n";

        std::cout << "Warnings:\n";
        if (summary.warnings.empty()) {
            std::cout << "- None\n";
        } else {
            for (const auto& warning : summary.warnings) {
                std::cout << "- " << warning << '\n';
            }
        }
    }

private:
    void print_progress(const repolens::FileMetadata& file, int processed, int total, const UpdateSummary& summary)
    {
        const int percent = total == 0 ? 100 : (std::max(0, std::min(processed, total)) * 100) / total;
        std::cout
            << progress_bar(processed, total) << ' ' << percent << "%  "
            << std::max(0, std::min(processed, total)) << " / " << total << '\n'
            << "Current file:\n"
            << file.relative_path << '\n'
            << "Running totals:\n"
            << "Added:      " << summary.added << '\n'
            << "Modified:   " << summary.modified << '\n'
            << "Deleted:    " << summary.deleted << '\n'
            << "Unchanged:  " << summary.unchanged << '\n'
            << "Reindexed:  " << summary.files_reindexed << '\n'
            << "Parsed:     " << summary.parse_counts.parsed << '\n'
            << "Failed:     " << (summary.failed + summary.parse_counts.failed) << "\n\n";
    }

    bool verbose_ = false;
};

void parse_changed_file(
    repolens::SqliteDatabase& database,
    const repolens::InterpreterRegistry& interpreters,
    long long repository_id,
    long long file_id,
    const repolens::FileMetadata& file,
    ParseCounts& counts,
    UpdateSummary& summary,
    bool lite_mode)
{
    const auto* interpreter = interpreters.find_for_file(file);
    if (!interpreter) {
        ++counts.skipped;
        return;
    }

    try {
        const auto result = interpreter->parse_file(file);
        if (result.success) {
            const auto stats = database.save_parse_result(repository_id, file_id, result, lite_mode);
            summary.symbols_added += stats.symbols_inserted;
            summary.symbols_updated += std::min(stats.symbols_inserted, stats.symbols_deleted);
            summary.symbols_deleted += std::max(0, stats.symbols_deleted - stats.symbols_inserted);
            summary.symbols_deactivated += stats.symbols_deactivated;
            ++counts.parsed;
        } else {
            const auto stats = database.save_parse_result(repository_id, file_id, result, lite_mode);
            summary.symbols_added += stats.symbols_inserted;
            summary.symbols_updated += std::min(stats.symbols_inserted, stats.symbols_deleted);
            summary.symbols_deleted += std::max(0, stats.symbols_deleted - stats.symbols_inserted);
            summary.symbols_deactivated += stats.symbols_deactivated;
            ++counts.failed;
        }
    } catch (const std::exception&) {
        ++counts.failed;
    }
}

void register_all_interpreters(repolens::InterpreterRegistry& interpreters)
{
    interpreters.register_interpreter(std::make_unique<repolens::CSharpInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::XmlInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::CppInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::BuildFileInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::WebInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::PythonInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::MatlabInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::JvmInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::GoInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::RustInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::PhpInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::RubyInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::ShellInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::SqlInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::DevOpsInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::AppleInterpreter>());
    interpreters.register_interpreter(std::make_unique<repolens::RInterpreter>());
}

std::filesystem::path database_path_from_index(const std::filesystem::path& index_dir)
{
    const auto index_root = canonical_existing_directory(index_dir, "index_dir");
    const auto database_path = index_root / "repolens.db";
    if (!std::filesystem::exists(database_path)) {
        throw std::runtime_error("RepoLens database does not exist: " + database_path.string());
    }

    return database_path;
}

UpdateSummary update_index(
    const std::filesystem::path& index_dir,
    UpdateProgressReporter& reporter,
    const repolens::ScanOptions& scan_options,
    bool lite_mode,
    bool closed_file_set = true,
    bool force_parse_all = false,
    bool similarity_prioritization = false);

UpdateSummary update_index(const std::filesystem::path& index_dir, UpdateProgressReporter& reporter)
{
    return update_index(index_dir, reporter, {}, false);
}

UpdateSummary update_index(
    const std::filesystem::path& index_dir,
    UpdateProgressReporter& reporter,
    const repolens::ScanOptions& scan_options,
    bool lite_mode,
    bool closed_file_set,
    bool force_parse_all,
    bool similarity_prioritization)
{
    const auto started = std::chrono::system_clock::now();
    const auto index_root = canonical_existing_directory(index_dir, "index_dir");
    const auto database_path = index_root / "repolens.db";

    if (!std::filesystem::exists(database_path)) {
        throw std::runtime_error("RepoLens database does not exist: " + database_path.string());
    }

    UpdateSummary summary;
    summary.index_root = index_root.string();
    summary.database_path = database_path.string();
    summary.started_at = format_timestamp(started);
    summary.database_size_before = file_size_or_zero(database_path);
    summary.lite_mode = lite_mode;

    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    if (lite_mode) {
        database.prune_lite_metadata();
    }

    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    const auto repo_root = canonical_existing_directory(status->repo_root, "repo_root");
    validate_external_index(repo_root, index_root);
    summary.repo_root = repo_root.string();

    reporter.on_scan_started(summary);

    const auto scan_result = repolens::scan_repository(repo_root, scan_options);
    auto similarity_groups = cluster_scanned_files(scan_result.files);
    auto ordered_scanned_files = similarity_prioritization ? order_files_by_similarity(scan_result.files, similarity_groups) : scan_result.files;
    const auto& scanned_files = ordered_scanned_files;
    summary.similarity_prioritization_enabled = similarity_prioritization;
    summary.similarity_group_summaries = similarity_groups;
    summary.similarity_groups = static_cast<int>(similarity_groups.size());
    for (const auto& group : similarity_groups) {
        summary.largest_similarity_group = std::max(summary.largest_similarity_group, static_cast<int>(group.files.size()));
    }
    summary.discovered = scan_result.discovered_count;
    summary.ignored = scan_result.ignored_count;
    reporter.on_scan_completed(summary);

    auto stored_files = database.read_files(status->repository_id);
    const auto previous_folders = database.read_folder_fingerprints(status->repository_id);
    summary.repo_fingerprint_before = database.repository_fingerprint(status->repository_id);
    const auto current_folders = compute_folder_fingerprints(scanned_files);
    summary.changed_folders = changed_folder_paths(previous_folders, current_folders);
    summary.folders_changed = static_cast<int>(summary.changed_folders.size());
    summary.folders_tracked = static_cast<int>(current_folders.size());
    summary.repo_fingerprint_after = compute_repository_fingerprint(scanned_files, current_folders);
    summary.files_scanned = static_cast<int>(scanned_files.size());
    summary.symbols_before = database.count_active_symbols(status->repository_id);

    std::unordered_set<std::string> seen_paths;
    repolens::InterpreterRegistry interpreters;
    register_all_interpreters(interpreters);
    auto ensure_snapshot = [&]() {
        if (lite_mode) {
            return 0LL;
        }
        if (summary.snapshot_id == 0) {
            summary.snapshot_id = database.create_snapshot(status->repository_id);
        }
        return summary.snapshot_id;
    };

    const int total_files = static_cast<int>(scanned_files.size());
    for (const auto& scanned_file : scanned_files) {
        reporter.on_file_started(scanned_file, summary.processed + 1, total_files, summary);
        try {
            seen_paths.insert(scanned_file.relative_path);
            summary.source_bytes += scanned_file.size_bytes;

            const auto stored = stored_files.find(scanned_file.relative_path);

            if (stored == stored_files.end()) {
                const auto snapshot_id = ensure_snapshot();
                const long long file_id = database.upsert_file(status->repository_id, scanned_file, snapshot_id, true);
                stored_files.emplace(
                    scanned_file.relative_path,
                    repolens::StoredFile{file_id, scanned_file.relative_path, scanned_file.content_hash, scanned_file.similarity_signature, true});
                if (!lite_mode) {
                    database.record_change(
                        status->repository_id,
                        snapshot_id,
                        "file",
                        file_id,
                        "file_added",
                        "",
                        scanned_file.content_hash,
                        "",
                        scanned_file.relative_path);
                }
                parse_changed_file(database, interpreters, status->repository_id, file_id, scanned_file, summary.parse_counts, summary, lite_mode);
                ++summary.files_reindexed;
                summary.reindexed_files.push_back(scanned_file.relative_path);
                ++summary.added;
            } else if (!stored->second.is_active) {
                const auto snapshot_id = ensure_snapshot();
                const long long file_id = database.upsert_file(status->repository_id, scanned_file, snapshot_id, false);
                stored->second.id = file_id;
                stored->second.content_hash = scanned_file.content_hash;
                stored->second.is_active = true;
                if (!lite_mode) {
                    database.record_change(
                        status->repository_id,
                        snapshot_id,
                        "file",
                        file_id,
                        "file_added",
                        "",
                        scanned_file.content_hash,
                        "",
                        scanned_file.relative_path);
                }
                parse_changed_file(database, interpreters, status->repository_id, file_id, scanned_file, summary.parse_counts, summary, lite_mode);
                ++summary.files_reindexed;
                summary.reindexed_files.push_back(scanned_file.relative_path);
                ++summary.added;
            } else if (stored->second.content_hash != scanned_file.content_hash || force_parse_all) {
                const bool content_changed = stored->second.content_hash != scanned_file.content_hash;
                long long file_id = stored->second.id;
                if (content_changed) {
                    const auto snapshot_id = ensure_snapshot();
                    file_id = database.upsert_file(status->repository_id, scanned_file, snapshot_id, false);
                    if (!lite_mode) {
                        database.record_change(
                            status->repository_id,
                            snapshot_id,
                            "file",
                            file_id,
                            "file_modified",
                            stored->second.content_hash,
                            scanned_file.content_hash,
                            scanned_file.relative_path,
                            scanned_file.relative_path);
                    }
                    stored->second.id = file_id;
                    stored->second.content_hash = scanned_file.content_hash;
                    ++summary.modified;
                } else {
                    ++summary.unchanged;
                }
                parse_changed_file(database, interpreters, status->repository_id, file_id, scanned_file, summary.parse_counts, summary, lite_mode);
                ++summary.files_reindexed;
                summary.reindexed_files.push_back(scanned_file.relative_path);
            } else {
                ++summary.unchanged;
            }

            ++summary.processed;
            reporter.on_file_completed(scanned_file, summary.processed, total_files, summary);
        } catch (const std::exception& error) {
            ++summary.processed;
            ++summary.failed;
            summary.warnings.push_back("Failed to process " + scanned_file.relative_path + ": " + error.what());
            reporter.on_file_failed(scanned_file, error.what(), summary.processed, total_files, summary);
        }
    }

    if (closed_file_set) {
        for (const auto& [relative_path, stored_file] : stored_files) {
            if (!stored_file.is_active || seen_paths.find(relative_path) != seen_paths.end()) {
                continue;
            }

            const auto snapshot_id = ensure_snapshot();
            database.mark_file_deleted(stored_file.id, snapshot_id);
            summary.symbols_deactivated += database.mark_symbols_inactive_for_file(stored_file.id);
            if (!lite_mode) {
                database.record_change(
                    status->repository_id,
                    snapshot_id,
                    "file",
                    stored_file.id,
                    "file_deleted",
                    stored_file.content_hash,
                    "",
                    relative_path,
                    "");
            }
            summary.deleted_files.push_back(relative_path);
            ++summary.deleted;
        }
    }

    database.replace_folder_fingerprints(status->repository_id, current_folders);
    database.update_repository_fingerprint(status->repository_id, summary.repo_fingerprint_after);
    database.update_last_indexed_at(status->repository_id);
    summary.symbols_after = database.count_active_symbols(status->repository_id);
    if (summary.parse_counts.failed > 0) {
        summary.warnings.push_back(std::to_string(summary.parse_counts.failed) + " files could not be parsed.");
    }

    const auto finished = std::chrono::system_clock::now();
    summary.finished_at = format_timestamp(finished);
    summary.elapsed_seconds = std::chrono::duration<double>(finished - started).count();
    summary.database_size_after = file_size_or_zero(database_path);
    reporter.on_update_completed(summary);
    return summary;
}

UpdateSummary update_index(const std::filesystem::path& index_dir)
{
    NullProgressReporter reporter;
    return update_index(index_dir, reporter, {}, false);
}

int run_init(int argc, char* argv[])
{
    if (argc != 5 || std::string_view{argv[3]} != "--index-dir") {
        throw std::runtime_error("Usage: repolens init <repo_path> --index-dir <index_path>");
    }

    const auto repo_root = canonical_existing_directory(argv[2], "repo_path");
    const auto requested_index_root = std::filesystem::absolute(std::filesystem::path{argv[4]}).lexically_normal();
    validate_external_index(repo_root, requested_index_root);
    const auto index_root = prepare_index_directory(requested_index_root);
    const auto database_path = index_root / "repolens.db";

    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    database.insert_repository(repo_root, index_root, schema_version);

    std::cout
        << "Initialized RepoLens index\n"
        << "Repo root: " << repo_root.string() << '\n'
        << "Index dir: " << index_root.string() << '\n'
        << "Database: " << database_path.string() << '\n';

    return 0;
}

struct StagedIndexStatus {
    std::filesystem::path stage_root;
    std::filesystem::path stage_database;
    std::filesystem::path backup_database;
    bool stage_root_exists = false;
    bool stage_database_exists = false;
    bool backup_database_exists = false;
};

StagedIndexStatus staged_index_status_for(const std::filesystem::path& index_root)
{
    StagedIndexStatus status;
    status.stage_root = index_root.parent_path() / (index_root.filename().string() + ".stage");
    status.stage_database = status.stage_root / "repolens.db";
    status.backup_database = index_root / "repolens.db.previous";
    status.stage_root_exists = std::filesystem::exists(status.stage_root);
    status.stage_database_exists = std::filesystem::exists(status.stage_database);
    status.backup_database_exists = std::filesystem::exists(status.backup_database);
    return status;
}
int run_status(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    bool show_scip = false;
    bool show_fingerprints = false;
    bool show_staged = false;
    bool show_similarity = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == "--show-scip") {
            show_scip = true;
        }
        if (std::string_view{argv[index]} == "--fingerprints") {
            show_fingerprints = true;
        }
        if (std::string_view{argv[index]} == "--staged") {
            show_staged = true;
        }
        if (std::string_view{argv[index]} == "--similarity") {
            show_similarity = true;
        }
    }
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens status --index-dir <index_path> [--show-scip] [--fingerprints] [--staged] [--similarity]");
    }

    const auto index_root = canonical_existing_directory(*index_dir, "index_dir");
    const auto database_path = index_root / "repolens.db";

    if (!std::filesystem::exists(database_path)) {
        throw std::runtime_error("RepoLens database does not exist: " + database_path.string());
    }

    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();

    if (!status) {
        std::cout
            << "Repo root: \n"
            << "Index dir: " << index_root.string() << '\n'
            << "Database: " << database_path.string() << '\n'
            << "Schema version: \n"
            << "Last indexed: \n";
        return 0;
    }

    std::cout
        << "Repo root: " << status->repo_root << '\n'
        << "Index dir: " << status->index_root << '\n'
        << "Database: " << database_path.string() << '\n'
        << "Schema version: " << status->schema_version << '\n'
        << "Last indexed: " << status->last_indexed_at << '\n';

    if (show_staged) {
        const auto staged = staged_index_status_for(index_root);
        std::cout
            << "Staged update status:\n"
            << "Stage root: " << staged.stage_root.string() << '\n'
            << "Stage root exists: " << (staged.stage_root_exists ? "yes" : "no") << '\n'
            << "Stage database exists: " << (staged.stage_database_exists ? "yes" : "no") << '\n'
            << "Backup database exists: " << (staged.backup_database_exists ? "yes" : "no") << '\n';
    }

    if (show_fingerprints) {
        const auto folders = database.read_folder_fingerprints(status->repository_id);
        std::vector<repolens::FolderFingerprint> ordered_folders;
        for (const auto& [folder_path, folder] : folders) {
            ordered_folders.push_back(folder);
        }
        std::sort(ordered_folders.begin(), ordered_folders.end(), [](const auto& left, const auto& right) {
            return left.folder_path < right.folder_path;
        });
        std::cout
            << "Repository fingerprint: " << database.repository_fingerprint(status->repository_id) << '\n'
            << "Folder fingerprints: " << ordered_folders.size() << '\n';
        for (const auto& folder : ordered_folders) {
            std::cout << "- " << folder.folder_path << " " << folder.fingerprint << " files=" << folder.file_count << " bytes=" << folder.total_size << '\n';
        }
    }

    if (show_similarity) {
        const auto files = database.read_similarity_files(status->repository_id);
        std::vector<std::pair<std::string, std::string>> records;
        for (const auto& file : files) {
            records.emplace_back(file.relative_path, file.similarity_signature);
        }
        const auto groups = cluster_similarity_records(records);
        int largest_group = 0;
        for (const auto& group : groups) {
            largest_group = std::max(largest_group, static_cast<int>(group.files.size()));
        }
        std::cout
            << "Similarity status:\n"
            << "Files with signatures: " << files.size() << '\n'
            << "Similarity groups: " << groups.size() << '\n'
            << "Largest group: " << largest_group << '\n';
        for (const auto& group : groups) {
            if (group.files.size() < 2) {
                continue;
            }
            std::cout << "- group " << group.id << " size=" << group.files.size() << " signature=" << group.representative_signature << '\n';
            for (const auto& file : group.files) {
                std::cout << "  - " << file << '\n';
            }
        }
    }

    if (show_scip) {
        const auto scip = database.last_scip_import(status->repository_id);
        if (scip) {
            std::cout
                << "SCIP import: " << scip->source_path << '\n'
                << "SCIP imported at: " << scip->imported_at << '\n'
                << "SCIP symbols seen: " << scip->symbols_seen << '\n'
                << "SCIP symbols mapped: " << scip->symbols_mapped << '\n'
                << "SCIP symbols inserted: " << scip->symbols_inserted << '\n'
                << "SCIP references inserted: " << scip->references_inserted << '\n'
                << "SCIP relationships inserted: " << scip->relationships_inserted << '\n'
                << "SCIP unresolved references: " << scip->unresolved_references << '\n'
                << "SCIP conflicts: " << scip->conflicts << '\n';
        } else {
            std::cout << "SCIP import: none\n";
        }
    }

    return 0;
}

bool has_flag(int argc, char* argv[], std::string_view flag)
{
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == flag) {
            return true;
        }
    }

    return false;
}

void print_update_compact(const UpdateSummary& summary)
{
    std::cout
        << "Update complete\n"
        << "Mode: " << (summary.lite_mode ? "lite" : "full") << '\n'
        << "Added: " << summary.added << '\n'
        << "Modified: " << summary.modified << '\n'
        << "Deleted: " << summary.deleted << '\n'
        << "Unchanged: " << summary.unchanged << '\n'
        << "Processed: " << summary.processed << '\n'
        << "Files scanned: " << summary.files_scanned << '\n'
        << "Files re-indexed: " << summary.files_reindexed << '\n'
        << "Source bytes: " << summary.source_bytes << '\n'
        << "Failed: " << summary.failed << '\n'
        << "Folders changed: " << summary.folders_changed << '\n'
        << "Repository fingerprint: " << summary.repo_fingerprint_after << '\n'
        << "Similarity prioritization: " << (summary.similarity_prioritization_enabled ? "enabled" : "disabled") << '\n'
        << "Similarity groups: " << summary.similarity_groups << '\n'
        << "Largest similarity group: " << summary.largest_similarity_group << '\n'
        << "Symbols inserted: " << summary.symbols_added << '\n'
        << "Symbols updated: " << summary.symbols_updated << '\n'
        << "Symbols deactivated: " << summary.symbols_deactivated << '\n'
        << "Parsed: " << summary.parse_counts.parsed << '\n'
        << "Parse skipped: " << summary.parse_counts.skipped << '\n'
        << "Parse failed: " << summary.parse_counts.failed << '\n';
    if (summary.show_diff) {
        std::cout << "Re-indexed files:\n";
        if (summary.reindexed_files.empty()) {
            std::cout << "- None\n";
        } else {
            for (const auto& file : summary.reindexed_files) {
                std::cout << "- " << file << '\n';
            }
        }
        std::cout << "Deleted files:\n";
        if (summary.deleted_files.empty()) {
            std::cout << "- None\n";
        } else {
            for (const auto& file : summary.deleted_files) {
                std::cout << "- " << file << '\n';
            }
        }
        std::cout << "Similarity groups detail:\n";
        for (const auto& group : summary.similarity_group_summaries) {
            if (group.files.size() < 2) {
                continue;
            }
            std::cout << "- group " << group.id << " size=" << group.files.size() << " signature=" << group.representative_signature << '\n';
            for (const auto& file : group.files) {
                std::cout << "  - " << file << '\n';
            }
        }
        std::cout << "Changed folders:\n";
        if (summary.changed_folders.empty()) {
            std::cout << "- None\n";
        } else {
            for (const auto& folder : summary.changed_folders) {
                std::cout << "- " << folder << '\n';
            }
        }
    }
}

std::string update_json(const UpdateSummary& summary);
repolens::ScipImportSummary import_scip_index_file(const std::filesystem::path& index_dir, const std::filesystem::path& scip_path);
void print_scip_import_summary(const repolens::ScipImportSummary& summary);

std::string trim_config_line(std::string text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) {
        return "";
    }

    text = std::string(first, last);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

std::vector<std::filesystem::path> read_path_list(const std::filesystem::path& path)
{
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("Failed to read path list: " + path.string());
    }

    std::vector<std::filesystem::path> paths;
    std::string line;
    while (std::getline(file, line)) {
        auto text = trim_config_line(line);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        paths.emplace_back(std::move(text));
    }
    return paths;
}

std::filesystem::path executable_root(char* argv0)
{
    auto executable_path = std::filesystem::path{argv0};
    if (executable_path.is_relative()) {
        executable_path = std::filesystem::absolute(executable_path);
    }
    return std::filesystem::weakly_canonical(executable_path).parent_path();
}

std::filesystem::path common_existing_root(const std::vector<std::filesystem::path>& include_paths)
{
    std::vector<std::filesystem::path> roots;
    for (const auto& path : include_paths) {
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Included path does not exist: " + path.string());
        }

        auto canonical_path = std::filesystem::canonical(path);
        roots.push_back(std::filesystem::is_directory(canonical_path) ? canonical_path : canonical_path.parent_path());
    }

    if (roots.empty()) {
        throw std::runtime_error("include.txt must contain at least one existing file or directory.");
    }

    auto common = roots.front();
    for (std::size_t index = 1; index < roots.size(); ++index) {
        while (!common.empty() && !is_same_or_child_path(common, roots[index])) {
            const auto parent = common.parent_path();
            if (parent == common) {
                break;
            }
            common = parent;
        }
    }

    if (common.empty()) {
        throw std::runtime_error("Unable to infer a common root from include.txt.");
    }

    return common;
}

bool same_path_text(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return comparable_path_text(left) == comparable_path_text(right);
}

void ensure_updateroot_database(const std::filesystem::path& index_root, const std::filesystem::path& repo_root)
{
    std::filesystem::create_directories(index_root);
    const auto database_path = index_root / "repolens.db";
    repolens::SqliteDatabase database{database_path};
    database.create_schema();

    const auto status = database.read_repository_status();
    if (!status ||
        !same_path_text(std::filesystem::path{status->repo_root}, repo_root) ||
        !same_path_text(std::filesystem::path{status->index_root}, index_root)) {
        database.insert_repository(repo_root, index_root, schema_version);
    }
}

std::vector<std::filesystem::path> split_path_arguments(const std::string& paths)
{
    std::vector<std::filesystem::path> result;
    std::stringstream stream{paths};
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = std::find_if_not(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); });
        const auto last = std::find_if_not(item.rbegin(), item.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        if (first < last) {
            result.emplace_back(std::string{first, last});
        }
    }
    return result;
}

UpdateSummary update_selected_files(
    const std::filesystem::path& index_dir,
    const std::filesystem::path& repo_path,
    const std::vector<std::filesystem::path>& files,
    bool lite_mode,
    bool replace_missing,
    UpdateProgressReporter& reporter)
{
    if (files.empty()) {
        throw std::runtime_error("--files must contain at least one file path.");
    }

    const auto repo_root = canonical_existing_directory(repo_path, "repo_root");
    const auto requested_index_root = std::filesystem::absolute(index_dir).lexically_normal();
    validate_external_index(repo_root, requested_index_root);
    const auto index_root = prepare_index_directory(requested_index_root);
    ensure_updateroot_database(index_root, repo_root);

    repolens::ScanOptions scan_options;
    for (const auto& file : files) {
        const auto absolute_file = file.is_absolute()
            ? std::filesystem::absolute(file).lexically_normal()
            : (repo_root / file).lexically_normal();
        if (!std::filesystem::exists(absolute_file) || !std::filesystem::is_regular_file(absolute_file)) {
            throw std::runtime_error("File does not exist: " + absolute_file.string());
        }
        if (!is_same_or_child_path(repo_root, absolute_file)) {
            throw std::runtime_error("File is outside repo_root: " + absolute_file.string());
        }
        scan_options.include_paths.push_back(absolute_file);
    }

    return update_index(index_root, reporter, scan_options, lite_mode, replace_missing);
}

void copy_active_database_to_stage(const std::filesystem::path& index_root, const std::filesystem::path& stage_root)
{
    std::filesystem::remove_all(stage_root);
    std::filesystem::create_directories(stage_root);
    std::filesystem::copy_file(index_root / "repolens.db", stage_root / "repolens.db", std::filesystem::copy_options::overwrite_existing);
}

void validate_staged_database(const std::filesystem::path& stage_root)
{
    const auto staged_database = stage_root / "repolens.db";
    if (!std::filesystem::exists(staged_database) || std::filesystem::file_size(staged_database) == 0) {
        throw std::runtime_error("Staged database was not created: " + staged_database.string());
    }
    repolens::SqliteDatabase database{staged_database};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Staged database is missing repository metadata: " + staged_database.string());
    }
    const auto counts = database.count_rows();
    if (counts.repositories <= 0 || counts.files < 0 || counts.symbols < 0) {
        throw std::runtime_error("Staged database failed validation: " + staged_database.string());
    }
}

void promote_staged_database(const std::filesystem::path& index_root, const std::filesystem::path& stage_root)
{
    const auto active_database = index_root / "repolens.db";
    const auto staged_database = stage_root / "repolens.db";
    const auto backup_database = index_root / "repolens.db.previous";
    const auto promotion_database = index_root / "repolens.db.promote";

    validate_staged_database(stage_root);
    std::filesystem::remove(promotion_database);
    std::filesystem::copy_file(staged_database, promotion_database, std::filesystem::copy_options::overwrite_existing);
    validate_staged_database(index_root.parent_path() / index_root.filename());

    std::filesystem::remove(backup_database);
    std::filesystem::rename(active_database, backup_database);
    try {
        std::filesystem::rename(promotion_database, active_database);
        validate_staged_database(index_root);
        std::filesystem::remove(backup_database);
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(active_database, cleanup_error);
        if (std::filesystem::exists(backup_database)) {
            std::filesystem::rename(backup_database, active_database);
        }
        std::filesystem::remove(promotion_database, cleanup_error);
        throw;
    }
}

void maybe_fail_staged_update_for_test(const char* point)
{
    const char* requested = std::getenv("REPOLENS_TEST_STAGED_FAIL_AT");
    if (requested != nullptr && std::string_view{requested} == point) {
        throw std::runtime_error(std::string{"Simulated staged update failure at "} + point);
    }
}

UpdateSummary update_index_staged(
    const std::filesystem::path& index_dir,
    UpdateProgressReporter& reporter,
    const repolens::ScanOptions& scan_options,
    bool lite_only,
    bool similarity_prioritization = false)
{
    const auto index_root = canonical_existing_directory(index_dir, "index_dir");
    const auto database_path = index_root / "repolens.db";
    if (!std::filesystem::exists(database_path)) {
        throw std::runtime_error("RepoLens database does not exist: " + database_path.string());
    }

    const auto stage_root = staged_index_status_for(index_root).stage_root;
    try {
        copy_active_database_to_stage(index_root, stage_root);
        auto summary = update_index(stage_root, reporter, scan_options, true, true, false, similarity_prioritization);
        validate_staged_database(stage_root);
        maybe_fail_staged_update_for_test("after_lite_stage");
        promote_staged_database(index_root, stage_root);
        summary.index_root = index_root.string();
        summary.database_path = (index_root / "repolens.db").string();

        if (!lite_only) {
            copy_active_database_to_stage(index_root, stage_root);
            summary = update_index(stage_root, reporter, scan_options, false, true, true, similarity_prioritization);
            validate_staged_database(stage_root);
            maybe_fail_staged_update_for_test("after_full_stage");
            promote_staged_database(index_root, stage_root);
            summary.index_root = index_root.string();
            summary.database_path = (index_root / "repolens.db").string();
        }

        std::filesystem::remove_all(stage_root);
        return summary;
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(stage_root, cleanup_error);
        const auto backup_database = index_root / "repolens.db.previous";
        const auto active_database = index_root / "repolens.db";
        if (!std::filesystem::exists(active_database) && std::filesystem::exists(backup_database)) {
            std::filesystem::rename(backup_database, active_database, cleanup_error);
        }
        throw;
    }
}
int run_update(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens update --index-dir <index_path> [--format text|json] [--progress|--no-progress] [--quiet] [--verbose] [--lite] [--staged] [--show-diff] [--scip-index <path>]");
    }

    const auto format = read_option_string(argc, argv, "--format").value_or("text");
    if (format != "text" && format != "json") {
        throw std::runtime_error("--format must be text or json.");
    }

    const bool quiet = has_flag(argc, argv, "--quiet");
    const bool no_progress = quiet || has_flag(argc, argv, "--no-progress") || format == "json";
    const bool verbose = has_flag(argc, argv, "--verbose");
    const bool lite_mode = has_flag(argc, argv, "--lite");
    const bool staged = has_flag(argc, argv, "--staged");
    const bool show_diff = has_flag(argc, argv, "--show-diff");
    const auto scip_index = read_option_path(argc, argv, "--scip-index");
    const bool similarity_prioritization = has_flag(argc, argv, "--optimize-large-repo") && !has_flag(argc, argv, "--no-similarity-prioritization");

    if (no_progress) {
        NullProgressReporter reporter;
        auto summary = staged
            ? update_index_staged(*index_dir, reporter, {}, lite_mode, similarity_prioritization)
            : update_index(*index_dir, reporter, {}, lite_mode, true, false, similarity_prioritization);
        summary.show_diff = show_diff;
        std::optional<repolens::ScipImportSummary> scip_summary;
        if (scip_index) {
            scip_summary = import_scip_index_file(*index_dir, *scip_index);
            std::ostringstream warning;
            warning << "SCIP import references=" << scip_summary->references_inserted
                    << " relationships=" << scip_summary->relationships_inserted
                    << " conflicts=" << scip_summary->conflicts;
            summary.warnings.push_back(warning.str());
        }
        if (format == "json") {
            std::cout << update_json(summary);
        } else {
            print_update_compact(summary);
            if (scip_summary) {
                print_scip_import_summary(*scip_summary);
            }
        }
    } else {
        ConsoleProgressReporter reporter{verbose};
        UpdateSummary summary;
        if (staged) {
            summary = update_index_staged(*index_dir, reporter, {}, lite_mode, similarity_prioritization);
        } else {
            summary = update_index(*index_dir, reporter, {}, lite_mode, true, false, similarity_prioritization);
        }
        summary.show_diff = show_diff;
        if (show_diff) {
            print_update_compact(summary);
        }
        if (scip_index) {
            print_scip_import_summary(import_scip_index_file(*index_dir, *scip_index));
        }
    }

    return 0;
}
int run_update_files(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto repo_root = read_option_path(argc, argv, "--repo-root");
    const auto files_option = read_option_string(argc, argv, "--files");
    const auto format = read_option_string(argc, argv, "--format").value_or("text");
    if (!index_dir || !repo_root || !files_option || files_option->empty()) {
        throw std::runtime_error("Usage: repolens update-files --index-dir <index_path> --repo-root <repo_path> --files <a,b> [--lite] [--replace] [--format text|json]");
    }
    if (format != "text" && format != "json") {
        throw std::runtime_error("--format must be text or json.");
    }

    const bool lite_mode = has_flag(argc, argv, "--lite");
    const bool replace_missing = has_flag(argc, argv, "--replace");
    const bool no_progress = has_flag(argc, argv, "--quiet") || has_flag(argc, argv, "--no-progress") || format == "json";
    if (no_progress) {
        NullProgressReporter reporter;
        const auto summary = update_selected_files(*index_dir, *repo_root, split_path_arguments(*files_option), lite_mode, replace_missing, reporter);
        if (format == "json") {
            std::cout << update_json(summary);
        } else {
            print_update_compact(summary);
        }
    } else {
        ConsoleProgressReporter reporter{has_flag(argc, argv, "--verbose")};
        update_selected_files(*index_dir, *repo_root, split_path_arguments(*files_option), lite_mode, replace_missing, reporter);
    }

    return 0;
}

int run_updateroot(int argc, char* argv[])
{
    const auto root = executable_root(argv[0]);
    const auto include_file = read_option_path(argc, argv, "--include-file").value_or(root / "include.txt");
    const auto exclude_file_option = read_option_path(argc, argv, "--exclude-file");
    const auto exclude_file = exclude_file_option.value_or(root / "exclude.txt");
    const bool require_exclude_file = exclude_file_option.has_value();

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--lite" || argument == "--staged") {
            continue;
        }

        if (argument == "--include-file" || argument == "--exclude-file") {
            ++index;
            if (index >= argc || std::string_view{argv[index]}.rfind("--", 0) == 0) {
                throw std::runtime_error("Usage: repolens updateroot [--include-file <path>] [--exclude-file <path>] [--lite] [--staged]");
            }
            continue;
        }

        throw std::runtime_error("Usage: repolens updateroot [--include-file <path>] [--exclude-file <path>] [--lite] [--staged]");
    }

    if (!std::filesystem::exists(include_file)) {
        throw std::runtime_error("include path list was not found: " + include_file.string());
    }

    if (require_exclude_file && !std::filesystem::exists(exclude_file)) {
        throw std::runtime_error("exclude path list was not found: " + exclude_file.string());
    }

    const auto include_paths = read_path_list(include_file);
    const auto exclude_paths = std::filesystem::exists(exclude_file)
        ? read_path_list(exclude_file)
        : std::vector<std::filesystem::path>{};

    const auto repo_root = common_existing_root(include_paths);
    ensure_updateroot_database(root, repo_root);

    repolens::ScanOptions scan_options;
    scan_options.include_paths = include_paths;
    scan_options.exclude_paths = exclude_paths;

    const bool lite_mode = has_flag(argc, argv, "--lite");
    const bool staged = has_flag(argc, argv, "--staged");
    ConsoleProgressReporter reporter{false};
    const auto summary = staged
        ? update_index_staged(root, reporter, scan_options, lite_mode)
        : update_index(root, reporter, scan_options, lite_mode);

    std::cout
        << "\nUpdateroot configuration\n"
        << "------------------------\n"
        << "Executable root: " << root.string() << '\n'
        << "Include file:    " << include_file.string() << '\n'
        << "Exclude file:    " << (std::filesystem::exists(exclude_file) ? exclude_file.string() : "(not found)") << '\n'
        << "Included paths:  " << include_paths.size() << '\n'
        << "Excluded paths:  " << exclude_paths.size() << '\n'
        << "Repo root:       " << summary.repo_root << '\n'
        << "Mode:            " << (summary.lite_mode ? "lite" : "full") << '\n'
        << "Database:        " << summary.database_path << '\n';

    return 0;
}

std::string json_escape(const std::string& text)
{
    std::ostringstream escaped;
    for (const char character : text) {
        switch (character) {
        case '\\':
            escaped << "\\\\";
            break;
        case '"':
            escaped << "\\\"";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                const auto value = static_cast<unsigned char>(character);
                escaped << "\\u00";
                const char* hex = "0123456789abcdef";
                escaped << hex[(value >> 4) & 0x0f] << hex[value & 0x0f];
            } else {
                escaped << character;
            }
            break;
        }
    }
    return escaped.str();
}

void print_search_text(const std::vector<repolens::SearchResult>& results)
{
    if (results.empty()) {
        std::cout << "No results.\n";
        return;
    }

    for (const auto& result : results) {
        if (result.result_type == "file") {
            std::cout << "file  " << result.file_path << '\n';
            continue;
        }

        std::cout
            << result.kind << "  " << result.qualified_name
            << "  " << result.file_path << ':' << result.line_start;
        if (!result.signature.empty()) {
            std::cout << "  " << result.signature;
        }
        std::cout << '\n';
    }
}

void print_search_json(const std::vector<repolens::SearchResult>& results)
{
    std::cout << "{\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        std::cout
            << "    {"
            << "\"type\": \"" << json_escape(result.result_type) << "\", "
            << "\"kind\": \"" << json_escape(result.kind) << "\", "
            << "\"name\": \"" << json_escape(result.name) << "\", "
            << "\"qualified_name\": \"" << json_escape(result.qualified_name) << "\", "
            << "\"signature\": \"" << json_escape(result.signature) << "\", "
            << "\"file\": \"" << json_escape(result.file_path) << "\", "
            << "\"line_start\": " << result.line_start << ", "
            << "\"line_end\": " << result.line_end
            << "}";
        if (index + 1 < results.size()) {
            std::cout << ',';
        }
        std::cout << '\n';
    }
    std::cout << "  ]\n}\n";
}

std::string search_json(const std::vector<repolens::SearchResult>& results)
{
    std::ostringstream output;
    output << "{\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        output
            << "    {"
            << "\"type\": \"" << json_escape(result.result_type) << "\", "
            << "\"kind\": \"" << json_escape(result.kind) << "\", "
            << "\"name\": \"" << json_escape(result.name) << "\", "
            << "\"qualified_name\": \"" << json_escape(result.qualified_name) << "\", "
            << "\"signature\": \"" << json_escape(result.signature) << "\", "
            << "\"file\": \"" << json_escape(result.file_path) << "\", "
            << "\"line_start\": " << result.line_start << ", "
            << "\"line_end\": " << result.line_end
            << "}";
        if (index + 1 < results.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::vector<repolens::SearchResult> search_index(
    const std::filesystem::path& index_dir,
    const repolens::SearchOptions& options)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();

    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    return database.search(status->repository_id, options);
}

int run_search(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto query = read_option_string(argc, argv, "--query");
    if (!index_dir || !query || query->empty()) {
        throw std::runtime_error("Usage: repolens search --index-dir <index_path> --query <text> [--kind <kind>] [--limit <n>] [--partial] [--format text|json]");
    }

    repolens::SearchOptions options;
    options.query = *query;
    options.kind = read_option_string(argc, argv, "--kind").value_or("");
    options.limit = read_option_int(argc, argv, "--limit", 20);
    const auto format = read_option_string(argc, argv, "--format").value_or("text");

    if (format != "text" && format != "json") {
        throw std::runtime_error("--format must be text or json.");
    }

    const auto results = search_index(*index_dir, options);
    if (format == "json") {
        std::cout << search_json(results);
    } else {
        print_search_text(results);
    }

    return 0;
}

std::string fact_output_format(int argc, char* argv[])
{
    if (has_flag(argc, argv, "--json")) {
        return "json";
    }
    const auto format = read_option_string(argc, argv, "--format").value_or("text");
    if (format != "text" && format != "json") {
        throw std::runtime_error("--format must be text or json.");
    }
    return format;
}

std::optional<std::string> first_positional_after_command(int argc, char* argv[])
{
    for (int index = 2; index < argc; ++index) {
        const std::string_view value{argv[index]};
        if (!value.empty() && value[0] == '-') {
            ++index;
            continue;
        }
        return std::string{argv[index]};
    }
    return std::nullopt;
}

std::string fact_symbol_json_object(const repolens::FactSymbol& symbol, const std::string& indent)
{
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"stable_id\": \"" << json_escape(symbol.stable_id) << "\",\n"
        << indent << "  \"row_id\": " << symbol.row_id << ",\n"
        << indent << "  \"file\": \"" << json_escape(symbol.file_path) << "\",\n"
        << indent << "  \"absolute_path\": \"" << json_escape(symbol.absolute_path) << "\",\n"
        << indent << "  \"language\": \"" << json_escape(symbol.language) << "\",\n"
        << indent << "  \"kind\": \"" << json_escape(symbol.kind) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(symbol.name) << "\",\n"
        << indent << "  \"qualified_name\": \"" << json_escape(symbol.qualified_name) << "\",\n"
        << indent << "  \"signature\": \"" << json_escape(symbol.signature) << "\",\n"
        << indent << "  \"line_start\": " << symbol.line_start << ",\n"
        << indent << "  \"line_end\": " << symbol.line_end << ",\n"
        << indent << "  \"parent_scope\": \"" << json_escape(symbol.parent_scope) << "\"\n"
        << indent << "}";
    return output.str();
}

std::string fact_symbols_json(const std::vector<repolens::FactSymbol>& symbols)
{
    std::ostringstream output;
    output << "{\n  \"ambiguous\": " << (symbols.size() > 1 ? "true" : "false") << ",\n  \"symbols\": [\n";
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        output << fact_symbol_json_object(symbols[index], "    ");
        if (index + 1 < symbols.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

void print_fact_symbols_text(const std::vector<repolens::FactSymbol>& symbols)
{
    if (symbols.size() > 1) {
        std::cout << "Ambiguous symbol name; candidates:\n";
    }
    for (const auto& symbol : symbols) {
        std::cout
            << symbol.kind << "  " << (symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name)
            << "  " << symbol.file_path << ':' << symbol.line_start << '-' << symbol.line_end
            << "  id=" << symbol.stable_id << '\n';
        if (!symbol.signature.empty()) {
            std::cout << "  signature: " << symbol.signature << '\n';
        }
        if (!symbol.parent_scope.empty()) {
            std::cout << "  parent: " << symbol.parent_scope << '\n';
        }
    }
}

std::vector<repolens::FactSymbol> resolve_fact_symbols(
    const std::filesystem::path& index_dir,
    const std::string& symbol_name,
    const std::optional<std::string>& file_path)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    auto symbols = database.resolve_symbols(status->repository_id, symbol_name, file_path);
    if (symbols.empty()) {
        throw std::runtime_error("Symbol not found: " + symbol_name);
    }
    return symbols;
}

int run_resolve_symbol(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto symbol_name = read_option_string(argc, argv, "--name").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || symbol_name.empty()) {
        throw std::runtime_error("Usage: repolens resolve-symbol --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]");
    }

    const auto symbols = resolve_fact_symbols(*index_dir, symbol_name, read_option_string(argc, argv, "--file"));
    if (fact_output_format(argc, argv) == "json") {
        std::cout << fact_symbols_json(symbols);
    } else {
        print_fact_symbols_text(symbols);
    }
    return 0;
}

std::string snippet_json(const repolens::SourceSnippet& snippet)
{
    std::ostringstream output;
    output
        << "{\n"
        << "  \"file\": \"" << json_escape(snippet.file_path) << "\",\n"
        << "  \"absolute_path\": \"" << json_escape(snippet.absolute_path) << "\",\n"
        << "  \"language\": \"" << json_escape(snippet.language) << "\",\n"
        << "  \"line_start\": " << snippet.line_start << ",\n"
        << "  \"line_end\": " << snippet.line_end << ",\n"
        << "  \"code\": \"" << json_escape(snippet.code) << "\"\n"
        << "}\n";
    return output.str();
}

int run_snippet(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto file_path = read_option_string(argc, argv, "--file");
    const auto start_text = read_option_string(argc, argv, "--start");
    const auto end_text = read_option_string(argc, argv, "--end");
    if (!index_dir || !file_path || !start_text || !end_text) {
        throw std::runtime_error("Usage: repolens snippet --index-dir <index_path> --file <path> --start <n> --end <n> [--format text|json|--json]");
    }

    const auto database_path = database_path_from_index(*index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    const auto snippet = database.read_snippet(status->repository_id, *file_path, std::stoi(*start_text), std::stoi(*end_text));
    if (fact_output_format(argc, argv) == "json") {
        std::cout << snippet_json(snippet);
    } else {
        std::cout << snippet.code;
    }
    return 0;
}

int run_symbol_range(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto symbol_name = read_option_string(argc, argv, "--name").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || symbol_name.empty()) {
        throw std::runtime_error("Usage: repolens symbol-range --index-dir <index_path> <name> [--file <path>] [--format text|json|--json]");
    }

    const auto symbols = resolve_fact_symbols(*index_dir, symbol_name, read_option_string(argc, argv, "--file"));
    if (fact_output_format(argc, argv) == "json") {
        std::cout << fact_symbols_json(symbols);
    } else {
        for (const auto& symbol : symbols) {
            std::cout
                << (symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name)
                << "  " << symbol.file_path << ':' << symbol.line_start << '-' << symbol.line_end
                << "  id=" << symbol.stable_id << '\n';
        }
    }
    return 0;
}
std::string string_array_json(const std::vector<std::string>& values, const std::string& indent);

struct CompactOptions {
    int max_depth = 8;
    int budget_chars = 12000;
    bool include_private = false;
    bool json = false;
};

struct CompactNode {
    repolens::FactSymbol symbol;
    std::vector<int> children;
};

struct CompactView {
    std::string file_path;
    std::string absolute_path;
    std::string language;
    std::vector<CompactNode> nodes;
    std::vector<int> roots;
    std::vector<std::string> warnings;
};

bool is_compact_structural_kind(const std::string& kind)
{
    return kind == "namespace" || kind == "class" || kind == "struct" || kind == "interface" ||
        kind == "enum" || kind == "function" || kind == "method" || kind == "constructor" ||
        kind == "destructor" || kind == "property" || kind == "trait" || kind == "record" ||
        kind == "module";
}

bool is_private_symbol(const repolens::FactSymbol& symbol)
{
    return symbol.visibility == "private" || symbol.visibility == "protected private";
}

std::string trim_compact_signature(std::string signature)
{
    while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) {
        signature.pop_back();
    }
    if (!signature.empty() && (signature.back() == '{' || signature.back() == ';')) {
        signature.pop_back();
    }
    while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) {
        signature.pop_back();
    }
    return signature;
}
std::string compact_symbol_label(const repolens::FactSymbol& symbol)
{
    const auto display_name = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
    std::string signature = symbol.signature.empty() ? display_name : trim_compact_signature(symbol.signature);
    if (symbol.kind == "namespace") {
        signature = "namespace " + symbol.name;
    } else if (symbol.kind == "class" || symbol.kind == "struct" || symbol.kind == "interface" || symbol.kind == "enum") {
        signature = symbol.kind + " " + symbol.name;
    }
    return signature;
}

bool compact_symbol_has_elided_body(const repolens::FactSymbol& symbol)
{
    return symbol.kind == "function" || symbol.kind == "method" || symbol.kind == "constructor" ||
        symbol.kind == "destructor" || symbol.kind == "property" || symbol.kind == "class" ||
        symbol.kind == "struct" || symbol.kind == "interface";
}

CompactView build_compact_view_from_symbols(
    std::vector<repolens::FactSymbol> symbols,
    const CompactOptions& options,
    const std::optional<repolens::FactSymbol>& root_symbol = std::nullopt)
{
    CompactView view;
    if (root_symbol) {
        view.file_path = root_symbol->file_path;
        view.absolute_path = root_symbol->absolute_path;
        view.language = root_symbol->language;
    } else if (!symbols.empty()) {
        view.file_path = symbols.front().file_path;
        view.absolute_path = symbols.front().absolute_path;
        view.language = symbols.front().language;
    }

    std::vector<repolens::FactSymbol> filtered;
    for (const auto& symbol : symbols) {
        if (!is_compact_structural_kind(symbol.kind)) {
            continue;
        }
        if (!options.include_private && is_private_symbol(symbol)) {
            continue;
        }
        if (root_symbol && symbol.file_path == root_symbol->file_path) {
            const bool in_range = symbol.line_start >= root_symbol->line_start && symbol.line_end <= root_symbol->line_end;
            if (symbol.row_id != root_symbol->row_id && !in_range) {
                continue;
            }
        }
        filtered.push_back(symbol);
    }

    std::sort(filtered.begin(), filtered.end(), [](const auto& left, const auto& right) {
        if (left.line_start != right.line_start) {
            return left.line_start < right.line_start;
        }
        if (left.line_end != right.line_end) {
            return left.line_end < right.line_end;
        }
        return left.qualified_name < right.qualified_name;
    });

    std::unordered_map<long long, int> index_by_row_id;
    for (const auto& symbol : filtered) {
        index_by_row_id.emplace(symbol.row_id, static_cast<int>(view.nodes.size()));
        CompactNode node;
        node.symbol = symbol;
        view.nodes.push_back(std::move(node));
    }

    for (int index = 0; index < static_cast<int>(view.nodes.size()); ++index) {
        int parent_index = -1;
        const auto stored_parent = index_by_row_id.find(view.nodes[index].symbol.parent_row_id);
        if (stored_parent != index_by_row_id.end() && stored_parent->second != index) {
            parent_index = stored_parent->second;
        } else {
            int best_span = 2147483647;
            const auto& child = view.nodes[index].symbol;
            for (int candidate_index = 0; candidate_index < static_cast<int>(view.nodes.size()); ++candidate_index) {
                if (candidate_index == index) {
                    continue;
                }
                const auto& candidate = view.nodes[candidate_index].symbol;
                if (candidate.line_start <= child.line_start && candidate.line_end >= child.line_end &&
                    (candidate.line_start != child.line_start || candidate.line_end != child.line_end)) {
                    const int span = candidate.line_end - candidate.line_start;
                    if (span < best_span) {
                        best_span = span;
                        parent_index = candidate_index;
                    }
                }
            }
        }

        if (parent_index >= 0) {
            view.nodes[parent_index].children.push_back(index);
        } else {
            view.roots.push_back(index);
        }
    }

    if (view.nodes.empty()) {
        view.warnings.push_back("No structural symbols found for compact view.");
    }
    return view;
}

CompactView load_compact_view_for_file(
    const std::filesystem::path& index_dir,
    const std::string& file_path,
    const CompactOptions& options)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }
    return build_compact_view_from_symbols(database.symbols_for_file(status->repository_id, file_path), options);
}

bool append_budgeted(std::ostringstream& output, const std::string& text, int& remaining)
{
    if (remaining <= 0) {
        return false;
    }
    if (static_cast<int>(text.size()) > remaining) {
        output << text.substr(0, static_cast<std::size_t>(remaining));
        remaining = 0;
        return false;
    }
    output << text;
    remaining -= static_cast<int>(text.size());
    return true;
}

void render_compact_text_node(
    const CompactView& view,
    int node_index,
    int depth,
    const CompactOptions& options,
    std::ostringstream& output,
    int& remaining,
    bool& truncated)
{
    if (depth > options.max_depth) {
        truncated = true;
        return;
    }
    const auto& node = view.nodes[static_cast<std::size_t>(node_index)];
    std::ostringstream line;
    line << std::string(static_cast<std::size_t>(depth * 2), ' ')
         << compact_symbol_label(node.symbol)
         << " [lines " << node.symbol.line_start << '-' << node.symbol.line_end << ']';
    if (compact_symbol_has_elided_body(node.symbol)) {
        line << " { ... }";
    }
    line << '\n';
    if (!append_budgeted(output, line.str(), remaining)) {
        truncated = true;
        return;
    }
    for (const int child : node.children) {
        render_compact_text_node(view, child, depth + 1, options, output, remaining, truncated);
        if (remaining <= 0) {
            return;
        }
    }
}

std::string compact_view_text(const std::vector<CompactView>& views, const CompactOptions& options)
{
    std::ostringstream output;
    int remaining = std::max(0, options.budget_chars);
    bool truncated = false;
    for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
        const auto& view = views[view_index];
        std::ostringstream header;
        header << "file: " << view.file_path << "\n\n";
        if (!append_budgeted(output, header.str(), remaining)) {
            truncated = true;
            break;
        }
        for (const int root : view.roots) {
            render_compact_text_node(view, root, 0, options, output, remaining, truncated);
            if (remaining <= 0) {
                break;
            }
        }
        for (const auto& warning : view.warnings) {
            append_budgeted(output, "warning: " + warning + "\n", remaining);
        }
        if (view_index + 1 < views.size()) {
            append_budgeted(output, "\n", remaining);
        }
    }
    if (truncated) {
        output << "... compact view truncated by depth or budget\n";
    }
    return output.str();
}

void render_compact_json_node(
    const CompactView& view,
    int node_index,
    int depth,
    const CompactOptions& options,
    std::ostringstream& output,
    const std::string& indent,
    bool& truncated)
{
    const auto& node = view.nodes[static_cast<std::size_t>(node_index)];
    output
        << indent << "{\n"
        << indent << "  \"kind\": \"" << json_escape(node.symbol.kind) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(node.symbol.name) << "\",\n"
        << indent << "  \"qualified_name\": \"" << json_escape(node.symbol.qualified_name) << "\",\n"
        << indent << "  \"signature\": \"" << json_escape(node.symbol.signature) << "\",\n"
        << indent << "  \"line_start\": " << node.symbol.line_start << ",\n"
        << indent << "  \"line_end\": " << node.symbol.line_end << ",\n"
        << indent << "  \"body_elided\": " << (compact_symbol_has_elided_body(node.symbol) ? "true" : "false") << ",\n"
        << indent << "  \"children\": [";
    if (depth >= options.max_depth && !node.children.empty()) {
        truncated = true;
    } else {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            output << '\n';
            render_compact_json_node(view, node.children[index], depth + 1, options, output, indent + "    ", truncated);
            if (index + 1 < node.children.size()) {
                output << ',';
            }
        }
    }
    if (!node.children.empty() && depth < options.max_depth) {
        output << '\n' << indent << "  ";
    }
    output << "]\n" << indent << "}";
}

std::string compact_view_json(const std::vector<CompactView>& views, const CompactOptions& options)
{
    std::ostringstream output;
    bool truncated = false;
    output << "{\n  \"compact\": true,\n  \"views\": [\n";
    for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
        const auto& view = views[view_index];
        output
            << "    {\n"
            << "      \"file\": \"" << json_escape(view.file_path) << "\",\n"
            << "      \"absolute_path\": \"" << json_escape(view.absolute_path) << "\",\n"
            << "      \"language\": \"" << json_escape(view.language) << "\",\n"
            << "      \"symbols\": [";
        for (std::size_t root_index = 0; root_index < view.roots.size(); ++root_index) {
            output << '\n';
            render_compact_json_node(view, view.roots[root_index], 0, options, output, "        ", truncated);
            if (root_index + 1 < view.roots.size()) {
                output << ',';
            }
        }
        if (!view.roots.empty()) {
            output << '\n' << "      ";
        }
        output << "],\n      \"warnings\": " << string_array_json(view.warnings, "        ") << "\n    }";
        if (view_index + 1 < views.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n  \"truncated\": " << (truncated ? "true" : "false") << "\n}\n";
    auto text = output.str();
    if (options.budget_chars > 0 && static_cast<int>(text.size()) > options.budget_chars) {
        std::ostringstream compact;
        compact
            << "{\n"
            << "  \"compact\": true,\n"
            << "  \"views\": [],\n"
            << "  \"truncated\": true,\n"
            << "  \"warnings\": [\"Compact JSON exceeded budget_chars before truncation-safe rendering. Increase --budget-chars.\"]\n"
            << "}\n";
        return compact.str();
    }
    return text;
}

std::vector<CompactView> compact_views_for_symbol(
    const std::filesystem::path& index_dir,
    const std::string& symbol_name,
    const CompactOptions& options)
{
    const auto roots = resolve_fact_symbols(index_dir, symbol_name, std::nullopt);
    std::vector<CompactView> views;
    for (const auto& root : roots) {
        auto symbols = load_compact_view_for_file(index_dir, root.file_path, options);
        views.push_back(build_compact_view_from_symbols(
            std::move(symbols.nodes.empty() ? std::vector<repolens::FactSymbol>{} : [&symbols]() {
                std::vector<repolens::FactSymbol> flattened;
                for (const auto& node : symbols.nodes) {
                    flattened.push_back(node.symbol);
                }
                return flattened;
            }()),
            options,
            root));
    }
    return views;
}

CompactOptions read_compact_options(int argc, char* argv[])
{
    CompactOptions options;
    options.max_depth = std::max(0, read_option_int(argc, argv, "--max-depth", 8));
    options.budget_chars = std::max(0, read_option_int(argc, argv, "--budget-chars", 12000));
    options.include_private = has_flag(argc, argv, "--include-private");
    options.json = fact_output_format(argc, argv) == "json";
    return options;
}

int run_compact_view(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto file_path = read_option_string(argc, argv, "--file");
    const auto symbol_name = read_option_string(argc, argv, "--symbol").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || (!file_path && symbol_name.empty())) {
        throw std::runtime_error("Usage: repolens compact-view --index-dir <index_path> [--file <path>|--symbol <name>] [--max-depth <n>] [--budget-chars <n>] [--include-private] [--format text|json|--json]");
    }

    const auto options = read_compact_options(argc, argv);
    std::vector<CompactView> views;
    if (file_path) {
        views.push_back(load_compact_view_for_file(*index_dir, *file_path, options));
    } else {
        views = compact_views_for_symbol(*index_dir, symbol_name, options);
    }

    if (options.json) {
        std::cout << compact_view_json(views, options);
    } else {
        std::cout << compact_view_text(views, options);
    }
    return 0;
}
std::string symbol_reference_json_object(const repolens::SymbolReferenceFact& reference, const std::string& indent)
{
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << reference.reference_id << ",\n"
        << indent << "  \"source_symbol_id\": " << reference.source_symbol_id << ",\n"
        << indent << "  \"target_symbol_id\": " << reference.target_symbol_id << ",\n"
        << indent << "  \"source_symbol\": \"" << json_escape(reference.source_symbol) << "\",\n"
        << indent << "  \"target_symbol\": \"" << json_escape(reference.target_symbol) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(reference.source_file) << "\",\n"
        << indent << "  \"language\": \"" << json_escape(reference.language) << "\",\n"
        << indent << "  \"line\": " << reference.line << ",\n"
        << indent << "  \"column\": " << reference.column << ",\n"
        << indent << "  \"reference_text\": \"" << json_escape(reference.reference_text) << "\",\n"
        << indent << "  \"relationship_type\": \"" << json_escape(reference.relationship_type) << "\",\n"
        << indent << "  \"confidence\": " << reference.confidence << ",\n"
        << indent << "  \"resolution_strategy\": \"" << json_escape(reference.resolution_strategy) << "\",\n"
        << indent << "  \"resolution_evidence\": \"" << json_escape(reference.resolution_evidence) << "\",\n"
        << indent << "  \"unresolved\": " << (reference.unresolved ? "true" : "false") << "\n"
        << indent << "}";
    return output.str();
}

std::string symbol_references_json(const std::vector<repolens::SymbolReferenceFact>& references)
{
    std::ostringstream output;
    output << "{\n  \"references\": [\n";
    for (std::size_t index = 0; index < references.size(); ++index) {
        output << symbol_reference_json_object(references[index], "    ");
        if (index + 1 < references.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::string symbol_relationship_json_object(const repolens::SymbolRelationshipFact& relationship, const std::string& indent)
{
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << relationship.relationship_id << ",\n"
        << indent << "  \"source_symbol_id\": " << relationship.source_symbol_id << ",\n"
        << indent << "  \"target_symbol_id\": " << relationship.target_symbol_id << ",\n"
        << indent << "  \"source_symbol\": \"" << json_escape(relationship.source_symbol) << "\",\n"
        << indent << "  \"target_symbol\": \"" << json_escape(relationship.target_symbol) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(relationship.source_file) << "\",\n"
        << indent << "  \"language\": \"" << json_escape(relationship.language) << "\",\n"
        << indent << "  \"line\": " << relationship.line << ",\n"
        << indent << "  \"column\": " << relationship.column << ",\n"
        << indent << "  \"relationship_type\": \"" << json_escape(relationship.relationship_type) << "\",\n"
        << indent << "  \"reference_text\": \"" << json_escape(relationship.reference_text) << "\",\n"
        << indent << "  \"confidence\": " << relationship.confidence << ",\n"
        << indent << "  \"resolution_strategy\": \"" << json_escape(relationship.resolution_strategy) << "\",\n"
        << indent << "  \"resolution_evidence\": \"" << json_escape(relationship.resolution_evidence) << "\",\n"
        << indent << "  \"unresolved\": " << (relationship.unresolved ? "true" : "false") << "\n"
        << indent << "}";
    return output.str();
}

std::string symbol_relationships_json(const std::vector<repolens::SymbolRelationshipFact>& relationships)
{
    std::ostringstream output;
    output << "{\n  \"relationships\": [\n";
    for (std::size_t index = 0; index < relationships.size(); ++index) {
        output << symbol_relationship_json_object(relationships[index], "    ");
        if (index + 1 < relationships.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

void print_symbol_references_text(const std::vector<repolens::SymbolReferenceFact>& references)
{
    if (references.empty()) {
        std::cout << "No references.\n";
        return;
    }
    for (const auto& reference : references) {
        std::cout
            << reference.source_file << ':' << reference.line << ':' << reference.column
            << "  " << reference.relationship_type
            << "  " << reference.source_symbol << " -> " << reference.target_symbol
            << "  text=" << reference.reference_text
            << "  confidence=" << reference.confidence
            << "  strategy=" << reference.resolution_strategy
            << "  evidence=" << reference.resolution_evidence;
        if (reference.unresolved) {
            std::cout << "  unresolved";
        }
        std::cout << '\n';
    }
}

void print_symbol_relationships_text(const std::vector<repolens::SymbolRelationshipFact>& relationships)
{
    if (relationships.empty()) {
        std::cout << "No relationships.\n";
        return;
    }
    for (const auto& relationship : relationships) {
        std::cout
            << relationship.relationship_type << "  "
            << relationship.source_symbol << " -> " << relationship.target_symbol
            << "  " << relationship.source_file << ':' << relationship.line << ':' << relationship.column
            << "  confidence=" << relationship.confidence
            << "  strategy=" << relationship.resolution_strategy
            << "  evidence=" << relationship.resolution_evidence;
        if (relationship.unresolved) {
            std::cout << "  unresolved";
        }
        std::cout << '\n';
    }
}

repolens::RepositoryStatus read_required_status(repolens::SqliteDatabase& database)
{
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }
    return *status;
}

int run_refs(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto symbol_name = read_option_string(argc, argv, "--symbol").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || symbol_name.empty()) {
        throw std::runtime_error("Usage: repolens refs --index-dir <index_path> <symbol> [--format text|json|--json]");
    }
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto references = database.references_for_symbol(status.repository_id, symbol_name);
    if (fact_output_format(argc, argv) == "json") {
        std::cout << symbol_references_json(references);
    } else {
        print_symbol_references_text(references);
    }
    return 0;
}

int run_relationships(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto symbol_name = read_option_string(argc, argv, "--symbol").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || symbol_name.empty()) {
        throw std::runtime_error("Usage: repolens relationships --index-dir <index_path> <symbol> [--type <kind>] [--format text|json|--json]");
    }
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto relationships = database.relationships_for_symbol(status.repository_id, symbol_name, read_option_string(argc, argv, "--type"));
    if (fact_output_format(argc, argv) == "json") {
        std::cout << symbol_relationships_json(relationships);
    } else {
        print_symbol_relationships_text(relationships);
    }
    return 0;
}

int run_unresolved_refs(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens unresolved-refs --index-dir <index_path> [--format text|json|--json]");
    }
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto references = database.unresolved_references(status.repository_id);
    if (fact_output_format(argc, argv) == "json") {
        std::cout << symbol_references_json(references);
    } else {
        print_symbol_references_text(references);
    }
    return 0;
}
double read_option_double(int argc, char* argv[], std::string_view option, double fallback)
{
    const auto value = read_option_string(argc, argv, option);
    if (!value) {
        return fallback;
    }
    return std::stod(*value);
}

struct TraceOptions {
    std::string direction = "callees";
    int max_depth = 2;
    int max_results = 100;
    int budget_chars = 12000;
    double min_confidence = 0.0;
    bool json = false;
};

struct TraceNode {
    repolens::FactSymbol symbol;
    int depth = 0;
};

struct TraceEdge {
    repolens::SymbolRelationshipFact relationship;
    int depth = 0;
};

struct TracePath {
    std::string text;
    int depth = 0;
    double confidence = 1.0;
};

struct TraceResult {
    std::string root_query;
    std::string direction;
    int max_depth = 0;
    bool truncated = false;
    std::vector<TraceNode> nodes;
    std::vector<TraceEdge> edges;
    std::vector<TracePath> paths;
};

std::string trace_symbol_name(const repolens::FactSymbol& symbol)
{
    return symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
}

std::string trace_node_json(const TraceNode& node, const std::string& indent)
{
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << node.symbol.row_id << ",\n"
        << indent << "  \"depth\": " << node.depth << ",\n"
        << indent << "  \"kind\": \"" << json_escape(node.symbol.kind) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(node.symbol.name) << "\",\n"
        << indent << "  \"qualified_name\": \"" << json_escape(node.symbol.qualified_name) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(node.symbol.file_path) << "\",\n"
        << indent << "  \"line_start\": " << node.symbol.line_start << ",\n"
        << indent << "  \"line_end\": " << node.symbol.line_end << "\n"
        << indent << "}";
    return output.str();
}

std::string trace_edge_json(const TraceEdge& edge, const std::string& indent)
{
    const auto& relationship = edge.relationship;
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << relationship.relationship_id << ",\n"
        << indent << "  \"depth\": " << edge.depth << ",\n"
        << indent << "  \"source_symbol_id\": " << relationship.source_symbol_id << ",\n"
        << indent << "  \"target_symbol_id\": " << relationship.target_symbol_id << ",\n"
        << indent << "  \"source_symbol\": \"" << json_escape(relationship.source_symbol) << "\",\n"
        << indent << "  \"target_symbol\": \"" << json_escape(relationship.target_symbol) << "\",\n"
        << indent << "  \"relationship_type\": \"" << json_escape(relationship.relationship_type) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(relationship.source_file) << "\",\n"
        << indent << "  \"line\": " << relationship.line << ",\n"
        << indent << "  \"column\": " << relationship.column << ",\n"
        << indent << "  \"confidence\": " << relationship.confidence << ",\n"
        << indent << "  \"resolution_strategy\": \"" << json_escape(relationship.resolution_strategy) << "\",\n"
        << indent << "  \"resolution_evidence\": \"" << json_escape(relationship.resolution_evidence) << "\",\n"
        << indent << "  \"unresolved\": " << (relationship.unresolved ? "true" : "false") << "\n"
        << indent << "}";
    return output.str();
}

std::string trace_json(const TraceResult& result, const TraceOptions& options)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"root_symbol\": \"" << json_escape(result.root_query) << "\",\n";
    output << "  \"direction\": \"" << json_escape(result.direction) << "\",\n";
    output << "  \"max_depth\": " << result.max_depth << ",\n";
    output << "  \"truncated\": " << (result.truncated ? "true" : "false") << ",\n";
    output << "  \"budget\": {\"requested_chars\": " << options.budget_chars << "},\n";
    output << "  \"nodes\": [\n";
    for (std::size_t index = 0; index < result.nodes.size(); ++index) {
        output << trace_node_json(result.nodes[index], "    ");
        if (index + 1 < result.nodes.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n  \"edges\": [\n";
    for (std::size_t index = 0; index < result.edges.size(); ++index) {
        output << trace_edge_json(result.edges[index], "    ");
        if (index + 1 < result.edges.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n  \"paths\": [\n";
    for (std::size_t index = 0; index < result.paths.size(); ++index) {
        output
            << "    {\"depth\": " << result.paths[index].depth
            << ", \"confidence\": " << result.paths[index].confidence
            << ", \"path\": \"" << json_escape(result.paths[index].text) << "\"}";
        if (index + 1 < result.paths.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    const auto text = output.str();
    if (options.budget_chars > 0 && static_cast<int>(text.size()) > options.budget_chars) {
        std::ostringstream compact;
        compact
            << "{\n"
            << "  \"root_symbol\": \"" << json_escape(result.root_query) << "\",\n"
            << "  \"direction\": \"" << json_escape(result.direction) << "\",\n"
            << "  \"max_depth\": " << result.max_depth << ",\n"
            << "  \"truncated\": true,\n"
            << "  \"budget\": {\"requested_chars\": " << options.budget_chars << "},\n"
            << "  \"nodes\": [],\n"
            << "  \"edges\": [],\n"
            << "  \"paths\": []\n"
            << "}\n";
        return compact.str();
    }
    return text;
}

std::string trace_text(const TraceResult& result, const TraceOptions& options)
{
    std::ostringstream output;
    int remaining = std::max(0, options.budget_chars);
    bool budget_truncated = false;
    append_budgeted(output, "Root: " + result.root_query + "\n", remaining);
    append_budgeted(output, "Direction: " + result.direction + "\n", remaining);
    append_budgeted(output, "Depth: " + std::to_string(result.max_depth) + "\n\n", remaining);
    append_budgeted(output, "Symbols reached:\n", remaining);
    for (const auto& node : result.nodes) {
        std::ostringstream line;
        line << "  [d" << node.depth << "] " << trace_symbol_name(node.symbol)
             << "  " << node.symbol.file_path << ':' << node.symbol.line_start << '-' << node.symbol.line_end << '\n';
        if (!append_budgeted(output, line.str(), remaining)) {
            budget_truncated = true;
            break;
        }
    }
    append_budgeted(output, "\nEdges:\n", remaining);
    for (const auto& edge : result.edges) {
        std::ostringstream line;
        line << "  [d" << edge.depth << "] " << edge.relationship.source_symbol
             << " --" << edge.relationship.relationship_type << "(" << edge.relationship.confidence << ")-> "
             << edge.relationship.target_symbol << "  " << edge.relationship.source_file << ':'
             << edge.relationship.line << ':' << edge.relationship.column;
        if (edge.relationship.unresolved) {
            line << " unresolved";
        }
        line << '\n';
        if (!append_budgeted(output, line.str(), remaining)) {
            budget_truncated = true;
            break;
        }
    }
    append_budgeted(output, "\nPaths:\n", remaining);
    for (const auto& path : result.paths) {
        if (!append_budgeted(output, "  " + path.text + "\n", remaining)) {
            budget_truncated = true;
            break;
        }
    }
    if (result.truncated || budget_truncated) {
        output << "... trace truncated by depth, result count, or budget\n";
    }
    return output.str();
}

TraceOptions read_trace_options(int argc, char* argv[])
{
    TraceOptions options;
    options.direction = read_option_string(argc, argv, "--direction").value_or("callees");
    if (options.direction != "callees" && options.direction != "callers" && options.direction != "both") {
        throw std::runtime_error("--direction must be callees, callers, or both.");
    }
    options.max_depth = std::max(0, read_option_int(argc, argv, "--depth", 2));
    options.max_results = std::max(1, read_option_int(argc, argv, "--max-results", 100));
    options.budget_chars = std::max(0, read_option_int(argc, argv, "--budget-chars", 12000));
    options.min_confidence = std::max(0.0, read_option_double(argc, argv, "--min-confidence", 0.0));
    options.json = fact_output_format(argc, argv) == "json";
    return options;
}

TraceResult trace_graph(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const std::string& root_query,
    const TraceOptions& options)
{
    TraceResult result;
    result.root_query = root_query;
    result.direction = options.direction;
    result.max_depth = options.max_depth;

    const auto roots = database.resolve_symbols(repository_id, root_query, std::nullopt);
    if (roots.empty()) {
        throw std::runtime_error("Symbol not found: " + root_query);
    }

    std::deque<std::tuple<long long, int, std::string, double>> queue;
    std::unordered_map<long long, int> best_depth_by_symbol;
    std::unordered_set<long long> seen_edges;

    auto add_node = [&](const repolens::FactSymbol& symbol, int depth) {
        const auto existing = best_depth_by_symbol.find(symbol.row_id);
        if (existing == best_depth_by_symbol.end()) {
            best_depth_by_symbol[symbol.row_id] = depth;
            TraceNode node;
            node.symbol = symbol;
            node.depth = depth;
            result.nodes.push_back(node);
            return true;
        }
        if (depth < existing->second) {
            existing->second = depth;
            for (auto& node : result.nodes) {
                if (node.symbol.row_id == symbol.row_id) {
                    node.depth = depth;
                    break;
                }
            }
            return true;
        }
        return false;
    };

    for (const auto& root : roots) {
        add_node(root, 0);
        queue.emplace_back(root.row_id, 0, trace_symbol_name(root), 1.0);
    }

    while (!queue.empty()) {
        const auto [symbol_id, depth, path_text, path_confidence] = queue.front();
        queue.pop_front();
        if (depth >= options.max_depth) {
            continue;
        }
        if (static_cast<int>(result.nodes.size() + result.edges.size()) >= options.max_results) {
            result.truncated = true;
            break;
        }

        const auto relationships = database.graph_relationships_for_symbol(repository_id, symbol_id, options.direction, options.min_confidence);
        for (const auto& relationship : relationships) {
            if (seen_edges.find(relationship.relationship_id) != seen_edges.end()) {
                continue;
            }
            seen_edges.insert(relationship.relationship_id);

            TraceEdge trace_edge;
            trace_edge.relationship = relationship;
            trace_edge.depth = depth + 1;
            result.edges.push_back(trace_edge);

            long long next_symbol_id = 0;
            std::string next_symbol_name;
            if (options.direction == "callers") {
                next_symbol_id = relationship.source_symbol_id;
                next_symbol_name = relationship.source_symbol;
            } else if (options.direction == "callees") {
                next_symbol_id = relationship.target_symbol_id;
                next_symbol_name = relationship.target_symbol;
            } else {
                if (relationship.source_symbol_id == symbol_id) {
                    next_symbol_id = relationship.target_symbol_id;
                    next_symbol_name = relationship.target_symbol;
                } else {
                    next_symbol_id = relationship.source_symbol_id;
                    next_symbol_name = relationship.source_symbol;
                }
            }

            const auto next_path_confidence = std::min(path_confidence, relationship.confidence);
            const auto next_path = path_text + " --" + relationship.relationship_type + "--> " + next_symbol_name;
            TracePath path;
            path.text = next_path;
            path.depth = depth + 1;
            path.confidence = next_path_confidence;
            result.paths.push_back(path);

            if (next_symbol_id == 0) {
                continue;
            }
            const auto next_symbol = database.symbol_by_id(repository_id, next_symbol_id);
            if (!next_symbol) {
                continue;
            }
            if (add_node(*next_symbol, depth + 1)) {
                queue.emplace_back(next_symbol_id, depth + 1, next_path, next_path_confidence);
            }

            if (static_cast<int>(result.nodes.size() + result.edges.size()) >= options.max_results) {
                result.truncated = true;
                break;
            }
        }
        if (result.truncated) {
            break;
        }
    }

    std::sort(result.nodes.begin(), result.nodes.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.symbol.file_path != right.symbol.file_path) {
            return left.symbol.file_path < right.symbol.file_path;
        }
        if (left.symbol.line_start != right.symbol.line_start) {
            return left.symbol.line_start < right.symbol.line_start;
        }
        return trace_symbol_name(left.symbol) < trace_symbol_name(right.symbol);
    });
    std::sort(result.edges.begin(), result.edges.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.relationship.confidence != right.relationship.confidence) {
            return left.relationship.confidence > right.relationship.confidence;
        }
        if (left.relationship.source_file != right.relationship.source_file) {
            return left.relationship.source_file < right.relationship.source_file;
        }
        if (left.relationship.line != right.relationship.line) {
            return left.relationship.line < right.relationship.line;
        }
        return left.relationship.relationship_id < right.relationship.relationship_id;
    });
    std::sort(result.paths.begin(), result.paths.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        return left.text < right.text;
    });
    return result;
}

int run_trace(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto root_symbol = read_option_string(argc, argv, "--symbol").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || root_symbol.empty()) {
        throw std::runtime_error("Usage: repolens trace --index-dir <index_path> <symbol> [--direction callees|callers|both] [--depth <n>] [--min-confidence <n>] [--max-results <n>] [--budget-chars <n>] [--format text|json|--json]");
    }

    const auto options = read_trace_options(argc, argv);
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto result = trace_graph(database, status.repository_id, root_symbol, options);
    if (options.json) {
        std::cout << trace_json(result, options);
    } else {
        std::cout << trace_text(result, options);
    }
    return 0;
}
struct ImpactOptions {
    int max_depth = 2;
    int max_results = 100;
    int budget_chars = 12000;
    bool include_paths = false;
    bool json = false;
};

struct ImpactNode {
    repolens::FactSymbol symbol;
    int depth = 0;
    double confidence = 1.0;
};

struct ImpactEdge {
    repolens::SymbolRelationshipFact relationship;
    int depth = 0;
};

struct ImpactPath {
    std::string text;
    int depth = 0;
    double confidence = 1.0;
};

struct ImpactResult {
    std::string root_target;
    std::string root_type;
    int max_depth = 0;
    bool truncated = false;
    std::vector<repolens::FactSymbol> roots;
    std::vector<ImpactNode> affected_symbols;
    std::vector<std::string> affected_files;
    std::vector<ImpactNode> direct_dependents;
    std::vector<ImpactNode> transitive_dependents;
    std::vector<ImpactEdge> edges;
    std::vector<ImpactPath> paths;
    std::vector<ImpactEdge> low_confidence_edges;
};

ImpactOptions read_impact_options(int argc, char* argv[])
{
    ImpactOptions options;
    options.max_depth = std::max(0, read_option_int(argc, argv, "--depth", 2));
    options.max_results = std::max(1, read_option_int(argc, argv, "--max-results", 100));
    options.budget_chars = std::max(0, read_option_int(argc, argv, "--budget-chars", 12000));
    options.include_paths = has_flag(argc, argv, "--include-paths");
    options.json = fact_output_format(argc, argv) == "json";
    return options;
}

std::string impact_symbol_name(const repolens::FactSymbol& symbol)
{
    return symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
}

std::string impact_node_json(const ImpactNode& node, const std::string& indent)
{
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << node.symbol.row_id << ",\n"
        << indent << "  \"depth\": " << node.depth << ",\n"
        << indent << "  \"confidence\": " << node.confidence << ",\n"
        << indent << "  \"kind\": \"" << json_escape(node.symbol.kind) << "\",\n"
        << indent << "  \"name\": \"" << json_escape(node.symbol.name) << "\",\n"
        << indent << "  \"qualified_name\": \"" << json_escape(node.symbol.qualified_name) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(node.symbol.file_path) << "\",\n"
        << indent << "  \"line_start\": " << node.symbol.line_start << ",\n"
        << indent << "  \"line_end\": " << node.symbol.line_end << "\n"
        << indent << "}";
    return output.str();
}

std::string impact_edge_json(const ImpactEdge& edge, const std::string& indent)
{
    const auto& relationship = edge.relationship;
    std::ostringstream output;
    output
        << indent << "{\n"
        << indent << "  \"id\": " << relationship.relationship_id << ",\n"
        << indent << "  \"depth\": " << edge.depth << ",\n"
        << indent << "  \"source_symbol_id\": " << relationship.source_symbol_id << ",\n"
        << indent << "  \"target_symbol_id\": " << relationship.target_symbol_id << ",\n"
        << indent << "  \"source_symbol\": \"" << json_escape(relationship.source_symbol) << "\",\n"
        << indent << "  \"target_symbol\": \"" << json_escape(relationship.target_symbol) << "\",\n"
        << indent << "  \"relationship_type\": \"" << json_escape(relationship.relationship_type) << "\",\n"
        << indent << "  \"file\": \"" << json_escape(relationship.source_file) << "\",\n"
        << indent << "  \"line\": " << relationship.line << ",\n"
        << indent << "  \"column\": " << relationship.column << ",\n"
        << indent << "  \"confidence\": " << relationship.confidence << ",\n"
        << indent << "  \"resolution_strategy\": \"" << json_escape(relationship.resolution_strategy) << "\",\n"
        << indent << "  \"resolution_evidence\": \"" << json_escape(relationship.resolution_evidence) << "\",\n"
        << indent << "  \"unresolved\": " << (relationship.unresolved ? "true" : "false") << "\n"
        << indent << "}";
    return output.str();
}

void sort_impact_nodes(std::vector<ImpactNode>& nodes)
{
    std::sort(nodes.begin(), nodes.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        if (left.symbol.file_path != right.symbol.file_path) {
            return left.symbol.file_path < right.symbol.file_path;
        }
        if (left.symbol.line_start != right.symbol.line_start) {
            return left.symbol.line_start < right.symbol.line_start;
        }
        return impact_symbol_name(left.symbol) < impact_symbol_name(right.symbol);
    });
}

ImpactResult analyze_impact(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const std::string& root_target,
    const std::optional<std::string>& file_path,
    const ImpactOptions& options)
{
    ImpactResult result;
    result.root_target = root_target;
    result.root_type = file_path ? std::string{"file"} : std::string{"symbol"};
    result.max_depth = options.max_depth;

    if (file_path) {
        result.roots = database.symbols_for_file(repository_id, *file_path);
        if (result.roots.empty()) {
            throw std::runtime_error("File path not found or has no indexed symbols: " + *file_path);
        }
    } else {
        result.roots = database.resolve_symbols(repository_id, root_target, std::nullopt);
        if (result.roots.empty()) {
            throw std::runtime_error("Symbol not found: " + root_target);
        }
    }

    std::unordered_set<long long> root_ids;
    std::unordered_map<long long, ImpactNode> best_nodes;
    std::unordered_set<long long> seen_edges;
    std::set<std::string> affected_files;
    std::set<std::string> seen_paths;
    std::deque<std::tuple<long long, int, std::string, double>> queue;

    for (const auto& root : result.roots) {
        root_ids.insert(root.row_id);
        queue.emplace_back(root.row_id, 0, impact_symbol_name(root), 1.0);
    }

    while (!queue.empty()) {
        const auto [symbol_id, depth, path_text, path_confidence] = queue.front();
        queue.pop_front();
        if (depth >= options.max_depth) {
            continue;
        }
        if (static_cast<int>(best_nodes.size() + result.edges.size()) >= options.max_results) {
            result.truncated = true;
            break;
        }

        const auto relationships = database.graph_relationships_for_symbol(repository_id, symbol_id, "callers", 0.0);
        for (const auto& relationship : relationships) {
            if (relationship.source_symbol_id == 0 || relationship.target_symbol_id == 0) {
                continue;
            }
            if (seen_edges.find(relationship.relationship_id) != seen_edges.end()) {
                continue;
            }
            seen_edges.insert(relationship.relationship_id);

            ImpactEdge edge;
            edge.relationship = relationship;
            edge.depth = depth + 1;
            result.edges.push_back(edge);
            if (relationship.confidence < 0.9 || relationship.unresolved) {
                result.low_confidence_edges.push_back(edge);
            }

            const auto next_confidence = std::min(path_confidence, relationship.confidence);
            const auto next_path = relationship.source_symbol + " --" + relationship.relationship_type + "--> " + path_text;
            if (seen_paths.insert(next_path).second) {
                ImpactPath path;
                path.text = next_path;
                path.depth = depth + 1;
                path.confidence = next_confidence;
                result.paths.push_back(path);
            }

            if (root_ids.find(relationship.source_symbol_id) != root_ids.end()) {
                continue;
            }
            const auto source_symbol = database.symbol_by_id(repository_id, relationship.source_symbol_id);
            if (!source_symbol) {
                continue;
            }
            affected_files.insert(source_symbol->file_path);

            const auto existing = best_nodes.find(source_symbol->row_id);
            const bool better = existing == best_nodes.end()
                || depth + 1 < existing->second.depth
                || (depth + 1 == existing->second.depth && next_confidence > existing->second.confidence);
            if (better) {
                ImpactNode node;
                node.symbol = *source_symbol;
                node.depth = depth + 1;
                node.confidence = next_confidence;
                best_nodes[source_symbol->row_id] = node;
                queue.emplace_back(source_symbol->row_id, depth + 1, next_path, next_confidence);
            }

            if (static_cast<int>(best_nodes.size() + result.edges.size()) >= options.max_results) {
                result.truncated = true;
                break;
            }
        }
        if (result.truncated) {
            break;
        }
    }

    for (const auto& [symbol_id, node] : best_nodes) {
        (void)symbol_id;
        result.affected_symbols.push_back(node);
        if (node.depth == 1) {
            result.direct_dependents.push_back(node);
        } else {
            result.transitive_dependents.push_back(node);
        }
    }
    sort_impact_nodes(result.affected_symbols);
    sort_impact_nodes(result.direct_dependents);
    sort_impact_nodes(result.transitive_dependents);

    std::sort(result.edges.begin(), result.edges.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.relationship.confidence != right.relationship.confidence) {
            return left.relationship.confidence > right.relationship.confidence;
        }
        if (left.relationship.source_file != right.relationship.source_file) {
            return left.relationship.source_file < right.relationship.source_file;
        }
        if (left.relationship.line != right.relationship.line) {
            return left.relationship.line < right.relationship.line;
        }
        return left.relationship.relationship_id < right.relationship.relationship_id;
    });
    std::sort(result.low_confidence_edges.begin(), result.low_confidence_edges.end(), [](const auto& left, const auto& right) {
        if (left.relationship.confidence != right.relationship.confidence) {
            return left.relationship.confidence < right.relationship.confidence;
        }
        return left.relationship.relationship_id < right.relationship.relationship_id;
    });
    std::sort(result.paths.begin(), result.paths.end(), [](const auto& left, const auto& right) {
        if (left.depth != right.depth) {
            return left.depth < right.depth;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        return left.text < right.text;
    });
    result.affected_files.assign(affected_files.begin(), affected_files.end());
    return result;
}

std::string impact_json(const ImpactResult& result, const ImpactOptions& options)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"root_target\": \"" << json_escape(result.root_target) << "\",\n";
    output << "  \"root_type\": \"" << json_escape(result.root_type) << "\",\n";
    output << "  \"max_depth\": " << result.max_depth << ",\n";
    output << "  \"truncated\": " << (result.truncated ? "true" : "false") << ",\n";
    output << "  \"budget\": {\"requested_chars\": " << options.budget_chars << "},\n";
    output << "  \"summary\": {\n";
    output << "    \"root_symbols\": " << result.roots.size() << ",\n";
    output << "    \"affected_files\": " << result.affected_files.size() << ",\n";
    output << "    \"affected_symbols\": " << result.affected_symbols.size() << ",\n";
    output << "    \"direct_dependents\": " << result.direct_dependents.size() << ",\n";
    output << "    \"transitive_dependents\": " << result.transitive_dependents.size() << ",\n";
    output << "    \"low_confidence_edges\": " << result.low_confidence_edges.size() << "\n";
    output << "  },\n";
    output << "  \"affected_files\": [";
    for (std::size_t index = 0; index < result.affected_files.size(); ++index) {
        if (index > 0) {
            output << ", ";
        }
        output << "\"" << json_escape(result.affected_files[index]) << "\"";
    }
    output << "],\n";
    auto write_nodes = [&](const char* name, const std::vector<ImpactNode>& nodes) {
        output << "  \"" << name << "\": [\n";
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            output << impact_node_json(nodes[index], "    ");
            if (index + 1 < nodes.size()) {
                output << ',';
            }
            output << '\n';
        }
        output << "  ],\n";
    };
    write_nodes("affected_symbols", result.affected_symbols);
    write_nodes("direct_dependents", result.direct_dependents);
    write_nodes("transitive_dependents", result.transitive_dependents);
    output << "  \"edges\": [\n";
    for (std::size_t index = 0; index < result.edges.size(); ++index) {
        output << impact_edge_json(result.edges[index], "    ");
        if (index + 1 < result.edges.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n";
    output << "  \"paths\": [\n";
    if (options.include_paths) {
        for (std::size_t index = 0; index < result.paths.size(); ++index) {
            output << "    {\"depth\": " << result.paths[index].depth
                   << ", \"confidence\": " << result.paths[index].confidence
                   << ", \"path\": \"" << json_escape(result.paths[index].text) << "\"}";
            if (index + 1 < result.paths.size()) {
                output << ',';
            }
            output << '\n';
        }
    }
    output << "  ],\n";
    output << "  \"confidence_notes\": {\n";
    output << "    \"low_confidence_threshold\": 0.9,\n";
    output << "    \"low_confidence_edges\": [\n";
    for (std::size_t index = 0; index < result.low_confidence_edges.size(); ++index) {
        output << impact_edge_json(result.low_confidence_edges[index], "      ");
        if (index + 1 < result.low_confidence_edges.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "    ]\n";
    output << "  }\n";
    output << "}\n";
    const auto text = output.str();
    if (options.budget_chars > 0 && static_cast<int>(text.size()) > options.budget_chars) {
        std::ostringstream compact;
        compact
            << "{\n"
            << "  \"root_target\": \"" << json_escape(result.root_target) << "\",\n"
            << "  \"root_type\": \"" << json_escape(result.root_type) << "\",\n"
            << "  \"max_depth\": " << result.max_depth << ",\n"
            << "  \"truncated\": true,\n"
            << "  \"budget\": {\"requested_chars\": " << options.budget_chars << "},\n"
            << "  \"summary\": {\"root_symbols\": " << result.roots.size()
            << ", \"affected_files\": " << result.affected_files.size()
            << ", \"affected_symbols\": " << result.affected_symbols.size()
            << ", \"direct_dependents\": " << result.direct_dependents.size()
            << ", \"transitive_dependents\": " << result.transitive_dependents.size()
            << ", \"low_confidence_edges\": " << result.low_confidence_edges.size() << "},\n"
            << "  \"affected_files\": [],\n"
            << "  \"affected_symbols\": [],\n"
            << "  \"direct_dependents\": [],\n"
            << "  \"transitive_dependents\": [],\n"
            << "  \"edges\": [],\n"
            << "  \"paths\": [],\n"
            << "  \"confidence_notes\": {\"low_confidence_threshold\": 0.9, \"low_confidence_edges\": []}\n"
            << "}\n";
        return compact.str();
    }
    return text;
}

std::string impact_text(const ImpactResult& result, const ImpactOptions& options)
{
    std::ostringstream output;
    int remaining = std::max(0, options.budget_chars);
    bool budget_truncated = false;
    append_budgeted(output, "Impact Analysis\n", remaining);
    append_budgeted(output, "Root: " + result.root_target + "\n", remaining);
    append_budgeted(output, "Root type: " + result.root_type + "\n", remaining);
    append_budgeted(output, "Depth: " + std::to_string(result.max_depth) + "\n", remaining);
    append_budgeted(output, "Affected files: " + std::to_string(result.affected_files.size()) + "\n", remaining);
    append_budgeted(output, "Affected symbols: " + std::to_string(result.affected_symbols.size()) + "\n", remaining);
    append_budgeted(output, "Direct dependents: " + std::to_string(result.direct_dependents.size()) + "\n", remaining);
    append_budgeted(output, "Transitive dependents: " + std::to_string(result.transitive_dependents.size()) + "\n\n", remaining);

    append_budgeted(output, "Affected files\n", remaining);
    for (const auto& file : result.affected_files) {
        if (!append_budgeted(output, "  " + file + "\n", remaining)) {
            budget_truncated = true;
            break;
        }
    }

    auto write_nodes = [&](const std::string& title, const std::vector<ImpactNode>& nodes) {
        append_budgeted(output, "\n" + title + "\n", remaining);
        for (const auto& node : nodes) {
            std::ostringstream line;
            line << "  [d" << node.depth << "] " << impact_symbol_name(node.symbol)
                 << " confidence=" << node.confidence
                 << " " << node.symbol.file_path << ':' << node.symbol.line_start << '-' << node.symbol.line_end << '\n';
            if (!append_budgeted(output, line.str(), remaining)) {
                budget_truncated = true;
                break;
            }
        }
    };
    write_nodes("Direct dependents", result.direct_dependents);
    write_nodes("Transitive dependents", result.transitive_dependents);

    if (options.include_paths) {
        append_budgeted(output, "\nRepresentative paths\n", remaining);
        for (const auto& path : result.paths) {
            std::ostringstream line;
            line << "  [d" << path.depth << "] confidence=" << path.confidence << " " << path.text << '\n';
            if (!append_budgeted(output, line.str(), remaining)) {
                budget_truncated = true;
                break;
            }
        }
    }

    append_budgeted(output, "\nConfidence notes\n", remaining);
    if (result.low_confidence_edges.empty()) {
        append_budgeted(output, "  No traversed edges below confidence 0.9.\n", remaining);
    } else {
        for (const auto& edge : result.low_confidence_edges) {
            std::ostringstream line;
            line << "  " << edge.relationship.source_symbol << " --" << edge.relationship.relationship_type
                 << "(" << edge.relationship.confidence << ")-> " << edge.relationship.target_symbol
                 << " strategy=" << edge.relationship.resolution_strategy << '\n';
            if (!append_budgeted(output, line.str(), remaining)) {
                budget_truncated = true;
                break;
            }
        }
    }
    if (result.truncated || budget_truncated) {
        output << "... impact output truncated by depth, result count, or budget\n";
    }
    return output.str();
}

int run_impact(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto file_path = read_option_string(argc, argv, "--file");
    const auto root_symbol = read_option_string(argc, argv, "--symbol").value_or(first_positional_after_command(argc, argv).value_or(""));
    if (!index_dir || (!file_path && root_symbol.empty())) {
        throw std::runtime_error("Usage: repolens impact --index-dir <index_path> [<symbol>|--symbol <symbol>|--file <path>] [--depth <n>] [--max-results <n>] [--budget-chars <n>] [--include-paths] [--format text|json|--json]");
    }
    const auto options = read_impact_options(argc, argv);
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto root_target = file_path ? *file_path : root_symbol;
    const auto result = analyze_impact(database, status.repository_id, root_target, file_path, options);
    if (options.json) {
        std::cout << impact_json(result, options);
    } else {
        std::cout << impact_text(result, options);
    }
    return 0;
}
struct ArchitectureOptions {
    bool communities = false;
    bool hubs = false;
    std::string level = "file";
    int top = 20;
    std::string seed;
    bool json = false;
};

struct ArchitectureNode {
    std::string id;
    std::string label;
    int in_degree = 0;
    int out_degree = 0;
    double incoming_weight = 0.0;
    double outgoing_weight = 0.0;
    double pagerank = 0.0;
    int community = -1;
};

struct ArchitectureProjectedEdge {
    long long relationship_id = 0;
    int source = 0;
    int target = 0;
    std::string source_id;
    std::string target_id;
    std::string relationship_type;
    std::string resolution_strategy;
    double confidence = 0.0;
    double weight = 0.0;
};

struct ArchitectureCommunity {
    int id = 0;
    int size = 0;
    double internal_weight = 0.0;
    std::vector<int> nodes;
};

struct ArchitectureResult {
    ArchitectureOptions options;
    std::vector<ArchitectureNode> nodes;
    std::vector<ArchitectureProjectedEdge> edges;
    std::vector<ArchitectureCommunity> communities;
    int largest_community_size = 0;
};

double architecture_edge_type_weight(const std::string& type)
{
    if (type == "calls") {
        return 3.0;
    }
    if (type == "inherits" || type == "implements") {
        return 2.5;
    }
    if (type == "imports" || type == "includes") {
        return 2.0;
    }
    if (type == "references" || type == "uses_type") {
        return 1.5;
    }
    if (type == "contains") {
        return 0.75;
    }
    return 1.0;
}

double architecture_edge_weight(const repolens::ArchitectureEdgeFact& edge)
{
    const double confidence = edge.confidence > 0.0 ? edge.confidence : 0.1;
    return architecture_edge_type_weight(edge.relationship_type) * confidence;
}

std::string architecture_symbol_label(const std::string& name, long long id)
{
    if (!name.empty()) {
        return name;
    }
    return "symbol:" + std::to_string(id);
}

int architecture_add_node(std::vector<ArchitectureNode>& nodes, std::map<std::string, int>& index_by_id, const std::string& id, const std::string& label)
{
    const auto existing = index_by_id.find(id);
    if (existing != index_by_id.end()) {
        return existing->second;
    }
    const int index = static_cast<int>(nodes.size());
    ArchitectureNode node;
    node.id = id;
    node.label = label.empty() ? id : label;
    nodes.push_back(node);
    index_by_id[id] = index;
    return index;
}

ArchitectureResult build_architecture_graph(const std::vector<repolens::ArchitectureEdgeFact>& facts, const ArchitectureOptions& options)
{
    ArchitectureResult result;
    result.options = options;
    std::map<std::string, int> index_by_id;
    std::set<std::tuple<std::string, std::string, std::string, long long>> seen_edges;

    for (const auto& fact : facts) {
        std::string source_id;
        std::string target_id;
        std::string source_label;
        std::string target_label;
        if (options.level == "symbol") {
            source_id = std::to_string(fact.source_symbol_id);
            target_id = std::to_string(fact.target_symbol_id);
            source_label = architecture_symbol_label(fact.source_symbol, fact.source_symbol_id);
            target_label = architecture_symbol_label(fact.target_symbol, fact.target_symbol_id);
        } else {
            source_id = fact.source_file;
            target_id = fact.target_file;
            source_label = fact.source_file;
            target_label = fact.target_file;
        }
        if (source_id.empty() || target_id.empty() || source_id == target_id) {
            continue;
        }
        const auto dedupe_key = std::make_tuple(source_id, target_id, fact.relationship_type, fact.relationship_id);
        if (!seen_edges.insert(dedupe_key).second) {
            continue;
        }
        const int source = architecture_add_node(result.nodes, index_by_id, source_id, source_label);
        const int target = architecture_add_node(result.nodes, index_by_id, target_id, target_label);
        ArchitectureProjectedEdge edge;
        edge.relationship_id = fact.relationship_id;
        edge.source = source;
        edge.target = target;
        edge.source_id = source_id;
        edge.target_id = target_id;
        edge.relationship_type = fact.relationship_type;
        edge.resolution_strategy = fact.resolution_strategy;
        edge.confidence = fact.confidence;
        edge.weight = architecture_edge_weight(fact);
        result.edges.push_back(edge);
        result.nodes[source].out_degree += 1;
        result.nodes[source].outgoing_weight += edge.weight;
        result.nodes[target].in_degree += 1;
        result.nodes[target].incoming_weight += edge.weight;
    }

    std::sort(result.nodes.begin(), result.nodes.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    index_by_id.clear();
    for (int index = 0; index < static_cast<int>(result.nodes.size()); ++index) {
        index_by_id[result.nodes[index].id] = index;
    }
    for (auto& edge : result.edges) {
        edge.source = index_by_id[edge.source_id];
        edge.target = index_by_id[edge.target_id];
    }
    std::sort(result.edges.begin(), result.edges.end(), [](const auto& left, const auto& right) {
        if (left.weight != right.weight) {
            return left.weight > right.weight;
        }
        if (left.source_id != right.source_id) {
            return left.source_id < right.source_id;
        }
        if (left.target_id != right.target_id) {
            return left.target_id < right.target_id;
        }
        if (left.relationship_type != right.relationship_type) {
            return left.relationship_type < right.relationship_type;
        }
        return left.relationship_id < right.relationship_id;
    });
    return result;
}

void compute_architecture_communities(ArchitectureResult& result)
{
    const int count = static_cast<int>(result.nodes.size());
    std::vector<std::vector<int>> adjacency(count);
    for (const auto& edge : result.edges) {
        adjacency[edge.source].push_back(edge.target);
        adjacency[edge.target].push_back(edge.source);
    }
    for (auto& neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    std::vector<int> component(count, -1);
    std::vector<ArchitectureCommunity> communities;
    for (int start = 0; start < count; ++start) {
        if (component[start] != -1) {
            continue;
        }
        const int community_id = static_cast<int>(communities.size());
        ArchitectureCommunity community;
        community.id = community_id;
        std::deque<int> queue;
        queue.push_back(start);
        component[start] = community_id;
        while (!queue.empty()) {
            const int node = queue.front();
            queue.pop_front();
            community.nodes.push_back(node);
            for (const int neighbor : adjacency[node]) {
                if (component[neighbor] == -1) {
                    component[neighbor] = community_id;
                    queue.push_back(neighbor);
                }
            }
        }
        std::sort(community.nodes.begin(), community.nodes.end(), [&](int left, int right) {
            return result.nodes[left].id < result.nodes[right].id;
        });
        community.size = static_cast<int>(community.nodes.size());
        communities.push_back(community);
    }

    for (const auto& edge : result.edges) {
        if (component[edge.source] >= 0 && component[edge.source] == component[edge.target]) {
            communities[component[edge.source]].internal_weight += edge.weight;
        }
    }
    std::sort(communities.begin(), communities.end(), [&](const auto& left, const auto& right) {
        if (left.size != right.size) {
            return left.size > right.size;
        }
        if (left.internal_weight != right.internal_weight) {
            return left.internal_weight > right.internal_weight;
        }
        const std::string left_first = left.nodes.empty() ? std::string{} : result.nodes[left.nodes.front()].id;
        const std::string right_first = right.nodes.empty() ? std::string{} : result.nodes[right.nodes.front()].id;
        return left_first < right_first;
    });
    for (int index = 0; index < static_cast<int>(communities.size()); ++index) {
        communities[index].id = index;
        for (const int node : communities[index].nodes) {
            result.nodes[node].community = index;
        }
        result.largest_community_size = std::max(result.largest_community_size, communities[index].size);
    }
    result.communities = communities;
}

bool architecture_seed_matches(const ArchitectureNode& node, const std::string& seed)
{
    if (seed.empty()) {
        return false;
    }
    if (node.id == seed || node.label == seed) {
        return true;
    }
    return node.id.find(seed) != std::string::npos || node.label.find(seed) != std::string::npos;
}

void compute_architecture_pagerank(ArchitectureResult& result)
{
    const int count = static_cast<int>(result.nodes.size());
    if (count == 0) {
        return;
    }
    std::vector<double> out_weight(count, 0.0);
    for (const auto& edge : result.edges) {
        out_weight[edge.source] += edge.weight;
    }

    std::vector<double> teleport(count, 1.0 / static_cast<double>(count));
    std::vector<int> seeds;
    for (int index = 0; index < count; ++index) {
        if (architecture_seed_matches(result.nodes[index], result.options.seed)) {
            seeds.push_back(index);
        }
    }
    if (!seeds.empty()) {
        std::fill(teleport.begin(), teleport.end(), 0.0);
        const double seed_weight = 1.0 / static_cast<double>(seeds.size());
        for (const int seed : seeds) {
            teleport[seed] = seed_weight;
        }
    }

    std::vector<double> rank = teleport;
    std::vector<double> next(count, 0.0);
    constexpr double damping = 0.85;
    for (int iteration = 0; iteration < 30; ++iteration) {
        for (int index = 0; index < count; ++index) {
            next[index] = (1.0 - damping) * teleport[index];
        }
        double dangling = 0.0;
        for (int index = 0; index < count; ++index) {
            if (out_weight[index] <= 0.0) {
                dangling += rank[index];
            }
        }
        for (int index = 0; index < count; ++index) {
            next[index] += damping * dangling * teleport[index];
        }
        for (const auto& edge : result.edges) {
            if (out_weight[edge.source] > 0.0) {
                next[edge.target] += damping * rank[edge.source] * (edge.weight / out_weight[edge.source]);
            }
        }
        rank.swap(next);
    }
    for (int index = 0; index < count; ++index) {
        result.nodes[index].pagerank = rank[index];
    }
}

std::vector<int> architecture_hub_order(const ArchitectureResult& result)
{
    std::vector<int> order(result.nodes.size());
    for (int index = 0; index < static_cast<int>(order.size()); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        const auto& left_node = result.nodes[left];
        const auto& right_node = result.nodes[right];
        if (left_node.pagerank != right_node.pagerank) {
            return left_node.pagerank > right_node.pagerank;
        }
        const double left_total = left_node.incoming_weight + left_node.outgoing_weight;
        const double right_total = right_node.incoming_weight + right_node.outgoing_weight;
        if (left_total != right_total) {
            return left_total > right_total;
        }
        return left_node.id < right_node.id;
    });
    return order;
}

ArchitectureOptions read_architecture_options(int argc, char* argv[])
{
    ArchitectureOptions options;
    options.communities = has_flag(argc, argv, "--communities");
    options.hubs = has_flag(argc, argv, "--hubs");
    if (!options.communities && !options.hubs) {
        options.communities = true;
        options.hubs = true;
    }
    options.level = read_option_string(argc, argv, "--level").value_or("file");
    if (options.level != "file" && options.level != "symbol") {
        throw std::runtime_error("--level must be file or symbol.");
    }
    options.top = std::max(1, read_option_int(argc, argv, "--top", 20));
    options.seed = read_option_string(argc, argv, "--seed").value_or("");
    options.json = fact_output_format(argc, argv) == "json";
    return options;
}

std::string architecture_json(const ArchitectureResult& result)
{
    std::ostringstream output;
    const auto hub_order = architecture_hub_order(result);
    output << "{\n";
    output << "  \"level\": \"" << json_escape(result.options.level) << "\",\n";
    output << "  \"seed\": \"" << json_escape(result.options.seed) << "\",\n";
    output << "  \"summary\": {\n";
    output << "    \"nodes\": " << result.nodes.size() << ",\n";
    output << "    \"edges\": " << result.edges.size() << ",\n";
    output << "    \"communities\": " << result.communities.size() << ",\n";
    output << "    \"largest_community_size\": " << result.largest_community_size << "\n";
    output << "  },\n";
    output << "  \"communities\": [\n";
    if (result.options.communities) {
        for (std::size_t index = 0; index < result.communities.size(); ++index) {
            const auto& community = result.communities[index];
            output << "    {\"id\": " << community.id
                   << ", \"size\": " << community.size
                   << ", \"internal_weight\": " << community.internal_weight
                   << ", \"nodes\": [";
            for (std::size_t node_index = 0; node_index < community.nodes.size(); ++node_index) {
                if (node_index > 0) {
                    output << ", ";
                }
                output << "\"" << json_escape(result.nodes[community.nodes[node_index]].label) << "\"";
            }
            output << "]}";
            if (index + 1 < result.communities.size()) {
                output << ',';
            }
            output << '\n';
        }
    }
    output << "  ],\n";
    output << "  \"hubs\": [\n";
    if (result.options.hubs) {
        const int limit = std::min(result.options.top, static_cast<int>(hub_order.size()));
        for (int rank = 0; rank < limit; ++rank) {
            const auto& node = result.nodes[hub_order[rank]];
            output << "    {\"rank\": " << (rank + 1)
                   << ", \"id\": \"" << json_escape(node.id)
                   << "\", \"label\": \"" << json_escape(node.label)
                   << "\", \"community\": " << node.community
                   << ", \"pagerank\": " << node.pagerank
                   << ", \"in_degree\": " << node.in_degree
                   << ", \"out_degree\": " << node.out_degree
                   << ", \"incoming_weight\": " << node.incoming_weight
                   << ", \"outgoing_weight\": " << node.outgoing_weight << "}";
            if (rank + 1 < limit) {
                output << ',';
            }
            output << '\n';
        }
    }
    output << "  ],\n";
    output << "  \"edges\": [\n";
    for (std::size_t index = 0; index < result.edges.size(); ++index) {
        const auto& edge = result.edges[index];
        output << "    {\"source\": \"" << json_escape(result.nodes[edge.source].label)
               << "\", \"target\": \"" << json_escape(result.nodes[edge.target].label)
               << "\", \"relationship_type\": \"" << json_escape(edge.relationship_type)
               << "\", \"weight\": " << edge.weight
               << ", \"confidence\": " << edge.confidence
               << ", \"resolution_strategy\": \"" << json_escape(edge.resolution_strategy) << "\"}";
        if (index + 1 < result.edges.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n";
    output << "}\n";
    return output.str();
}

std::string architecture_text(const ArchitectureResult& result)
{
    std::ostringstream output;
    output << "Architecture Analysis\n";
    output << "Level: " << result.options.level << '\n';
    if (!result.options.seed.empty()) {
        output << "Seed: " << result.options.seed << '\n';
    }
    output << "Nodes: " << result.nodes.size() << '\n';
    output << "Edges: " << result.edges.size() << '\n';
    output << "Communities: " << result.communities.size() << '\n';
    output << "Largest community size: " << result.largest_community_size << "\n\n";
    if (result.nodes.empty()) {
        output << "No architecture graph edges. Run update after indexing references, or inspect unresolved-refs.\n";
        return output.str();
    }
    if (result.options.communities) {
        output << "Communities\n";
        for (const auto& community : result.communities) {
            output << "  [" << community.id << "] size=" << community.size
                   << " weight=" << community.internal_weight << '\n';
            for (const int node_index : community.nodes) {
                output << "    " << result.nodes[node_index].label << '\n';
            }
        }
        output << '\n';
    }
    if (result.options.hubs) {
        output << "Hubs\n";
        const auto order = architecture_hub_order(result);
        const int limit = std::min(result.options.top, static_cast<int>(order.size()));
        for (int rank = 0; rank < limit; ++rank) {
            const auto& node = result.nodes[order[rank]];
            output << "  " << (rank + 1) << ". " << node.label
                   << " score=" << node.pagerank
                   << " in=" << node.in_degree
                   << " out=" << node.out_degree
                   << " community=" << node.community << '\n';
        }
    }
    return output.str();
}

int run_architecture(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens architecture --index-dir <index_path> [--communities|--hubs] [--level file|symbol] [--top <n>] [--seed <id-or-name>] [--format text|json|--json]");
    }
    const auto options = read_architecture_options(argc, argv);
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    auto result = build_architecture_graph(database.architecture_edges(status.repository_id), options);
    compute_architecture_communities(result);
    compute_architecture_pagerank(result);
    if (options.json) {
        std::cout << architecture_json(result);
    } else {
        std::cout << architecture_text(result);
    }
    return 0;
}
std::vector<std::string> split_symbols(const std::string& symbols)
{
    std::vector<std::string> result;
    std::stringstream stream{symbols};
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = std::find_if_not(item.begin(), item.end(), [](unsigned char c) { return std::isspace(c); });
        const auto last = std::find_if_not(item.rbegin(), item.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        if (first < last) {
            result.emplace_back(first, last);
        }
    }
    return result;
}

std::string read_line_range(const std::filesystem::path& path, int line_start, int line_end)
{
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("Failed to read source file: " + path.string());
    }

    std::ostringstream snippet;
    std::string line;
    int line_number = 1;
    while (std::getline(file, line)) {
        if (line_number >= line_start && line_number <= line_end) {
            snippet << line << '\n';
        }
        if (line_number > line_end) {
            break;
        }
        ++line_number;
    }

    return snippet.str();
}

void print_string_array_json(const std::vector<std::string>& values, const std::string& indent)
{
    std::cout << "[";
    if (!values.empty()) {
        std::cout << '\n';
        for (std::size_t index = 0; index < values.size(); ++index) {
            std::cout << indent << "\"" << json_escape(values[index]) << "\"";
            if (index + 1 < values.size()) {
                std::cout << ',';
            }
            std::cout << '\n';
        }
        std::cout << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    std::cout << "]";
}

std::string string_array_json(const std::vector<std::string>& values, const std::string& indent)
{
    std::ostringstream output;
    output << "[";
    if (!values.empty()) {
        output << '\n';
        for (std::size_t index = 0; index < values.size(); ++index) {
            output << indent << "\"" << json_escape(values[index]) << "\"";
            if (index + 1 < values.size()) {
                output << ',';
            }
            output << '\n';
        }
        output << indent.substr(0, indent.size() >= 2 ? indent.size() - 2 : 0);
    }
    output << "]";
    return output.str();
}

std::string ascii_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string human_kind_label(std::string kind)
{
    if (kind.empty()) {
        return "Symbol";
    }
    for (auto& ch : kind) {
        if (ch == '_' || ch == '-') {
            ch = ' ';
        }
    }
    bool capitalize_next = true;
    for (auto& ch : kind) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            capitalize_next = true;
            continue;
        }
        if (capitalize_next) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            capitalize_next = false;
        }
    }
    return kind;
}

bool kind_contains(const std::string& kind, const std::string& needle)
{
    return ascii_lower(kind).find(needle) != std::string::npos;
}

std::string parent_scope_from_qualified_name(const std::string& qualified_name, const std::string& name)
{
    if (qualified_name.empty() || name.empty() || qualified_name == name) {
        return "";
    }
    const std::vector<std::string> separators{"::", ".", "#"};
    for (const auto& separator : separators) {
        const auto suffix = separator + name;
        if (qualified_name.size() > suffix.size() &&
            qualified_name.compare(qualified_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return qualified_name.substr(0, qualified_name.size() - suffix.size());
        }
    }
    const auto position = qualified_name.find_last_of(".:#");
    return position == std::string::npos ? std::string{} : qualified_name.substr(0, position);
}

std::string line_range_text(int line_start, int line_end)
{
    if (line_start > 0 && line_end > 0) {
        return "lines " + std::to_string(line_start) + "-" + std::to_string(line_end);
    }
    if (line_start > 0) {
        return "line " + std::to_string(line_start);
    }
    return "unknown lines";
}

std::string deterministic_symbol_description(
    const std::string& kind,
    const std::string& name,
    const std::string& qualified_name,
    const std::string& parent_scope,
    const std::string& file_path,
    int line_start,
    int line_end)
{
    const auto label = human_kind_label(kind);
    const auto display_name = qualified_name.empty() ? name : qualified_name;
    std::ostringstream description;
    description << label << " `" << display_name << "`";
    if (!parent_scope.empty()) {
        description << " in scope `" << parent_scope << "`";
    }
    description << ", defined in `" << file_path << "`, " << line_range_text(line_start, line_end) << ".";
    return description.str();
}

std::string deterministic_symbol_description(const repolens::FactSymbol& symbol)
{
    const auto parent_scope = symbol.parent_scope.empty()
        ? parent_scope_from_qualified_name(symbol.qualified_name, symbol.name)
        : symbol.parent_scope;
    return deterministic_symbol_description(
        symbol.kind,
        symbol.name,
        symbol.qualified_name,
        parent_scope,
        symbol.file_path,
        symbol.line_start,
        symbol.line_end);
}

std::string deterministic_symbol_description(const repolens::ContextSymbolCandidate& symbol)
{
    return deterministic_symbol_description(
        symbol.kind,
        symbol.name,
        symbol.qualified_name,
        parent_scope_from_qualified_name(symbol.qualified_name, symbol.name),
        symbol.relative_path,
        symbol.line_start,
        symbol.line_end);
}

std::string pluralized_count(int count, const std::string& singular, const std::string& plural)
{
    return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

std::string deterministic_file_description(const std::string& file_path, const std::vector<repolens::FactSymbol>& symbols)
{
    int namespaces = 0;
    int classes = 0;
    int structs = 0;
    int interfaces = 0;
    int enums = 0;
    int methods = 0;
    int functions = 0;
    int fields = 0;

    for (const auto& symbol : symbols) {
        if (kind_contains(symbol.kind, "namespace")) {
            ++namespaces;
        } else if (kind_contains(symbol.kind, "interface")) {
            ++interfaces;
        } else if (kind_contains(symbol.kind, "class")) {
            ++classes;
        } else if (kind_contains(symbol.kind, "struct")) {
            ++structs;
        } else if (kind_contains(symbol.kind, "enum")) {
            ++enums;
        } else if (kind_contains(symbol.kind, "method") || kind_contains(symbol.kind, "constructor")) {
            ++methods;
        } else if (kind_contains(symbol.kind, "function")) {
            ++functions;
        } else if (kind_contains(symbol.kind, "field") || kind_contains(symbol.kind, "property")) {
            ++fields;
        }
    }

    std::vector<std::string> parts;
    if (namespaces > 0) parts.push_back(pluralized_count(namespaces, "namespace", "namespaces"));
    if (classes > 0) parts.push_back(pluralized_count(classes, "class", "classes"));
    if (structs > 0) parts.push_back(pluralized_count(structs, "struct", "structs"));
    if (interfaces > 0) parts.push_back(pluralized_count(interfaces, "interface", "interfaces"));
    if (enums > 0) parts.push_back(pluralized_count(enums, "enum declaration", "enum declarations"));
    if (methods > 0) parts.push_back(pluralized_count(methods, "method", "methods"));
    if (functions > 0) parts.push_back(pluralized_count(functions, "function", "functions"));
    if (fields > 0) parts.push_back(pluralized_count(fields, "field or property", "fields or properties"));

    std::ostringstream description;
    description << "File `" << file_path << "` contains ";
    if (parts.empty()) {
        description << "no indexed declarations";
    } else {
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (index > 0 && parts.size() > 2) {
                description << ",";
            }
            if (index > 0) {
                description << (index + 1 == parts.size() ? " and " : " ");
            }
            description << parts[index];
        }
    }
    description << ".";
    return description.str();
}

class ContextDescriptionProvider {
public:
    virtual ~ContextDescriptionProvider() = default;
    virtual std::string describe_file(const std::string& file_path, const std::vector<repolens::FactSymbol>& symbols) const = 0;
    virtual std::string describe_symbol(const repolens::FactSymbol& symbol) const = 0;
    virtual std::string describe_symbol(const repolens::ContextSymbolCandidate& symbol) const = 0;
};

class DeterministicContextDescriptionProvider final : public ContextDescriptionProvider {
public:
    std::string describe_file(const std::string& file_path, const std::vector<repolens::FactSymbol>& symbols) const override
    {
        return deterministic_file_description(file_path, symbols);
    }

    std::string describe_symbol(const repolens::FactSymbol& symbol) const override
    {
        return deterministic_symbol_description(symbol);
    }

    std::string describe_symbol(const repolens::ContextSymbolCandidate& symbol) const override
    {
        return deterministic_symbol_description(symbol);
    }
};

std::string situated_context_preamble(const std::string& description)
{
    return description.empty() ? std::string{} : "Context: " + description + "\n\n";
}

std::string description_json(const std::vector<repolens::ContextDescription>& descriptions)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"descriptions\": [\n";
    for (std::size_t index = 0; index < descriptions.size(); ++index) {
        const auto& item = descriptions[index];
        output << "    {\n";
        output << "      \"target_type\": \"" << json_escape(item.target_type) << "\",\n";
        output << "      \"target_id\": " << item.target_id << ",\n";
        output << "      \"target_key\": \"" << json_escape(item.target_key) << "\",\n";
        output << "      \"source\": \"" << json_escape(item.source) << "\",\n";
        output << "      \"updated_at\": \"" << json_escape(item.updated_at) << "\",\n";
        output << "      \"description\": \"" << json_escape(item.description) << "\"\n";
        output << "    }";
        if (index + 1 < descriptions.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n";
    output << "}\n";
    return output.str();
}
struct ContextItem {
    std::string requested_symbol;
    repolens::ContextSymbolCandidate symbol;
    std::string code;
    std::string relation_type;
    std::string source_qualified_name;
    std::string situated_description;
    int level = 0;
    bool related = false;
    bool truncated = false;
};

std::string situated_description_for_context_item(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const repolens::ContextSymbolCandidate& symbol,
    const ContextDescriptionProvider& provider)
{
    const auto stored = database.context_description_for(repository_id, "symbol", symbol.symbol_id);
    if (stored && !stored->description.empty()) {
        return stored->description;
    }
    return provider.describe_symbol(symbol);
}

void apply_situated_context(
    repolens::SqliteDatabase& database,
    long long repository_id,
    std::vector<ContextItem>& items,
    int& remaining_budget,
    std::vector<std::string>& warnings)
{
    DeterministicContextDescriptionProvider provider;
    for (auto& item : items) {
        if (item.situated_description.empty()) {
            item.situated_description = situated_description_for_context_item(database, repository_id, item.symbol, provider);
        }
        const auto preamble = situated_context_preamble(item.situated_description);
        if (preamble.empty()) {
            continue;
        }
        if (item.code.rfind("Context: ", 0) == 0) {
            continue;
        }
        if (remaining_budget > 0) {
            remaining_budget = std::max(0, remaining_budget - static_cast<int>(preamble.size()));
        } else if (!item.truncated) {
            warnings.push_back("Situated metadata exceeded remaining budget: " + item.symbol.qualified_name);
        }
        item.code = preamble + item.code;
    }
}
void fill_context_code(ContextItem& item, int& remaining_budget, std::vector<std::string>& warnings)
{
    if (remaining_budget > 0) {
        auto snippet = read_line_range(item.symbol.absolute_path, item.symbol.line_start, item.symbol.line_end);
        if (static_cast<int>(snippet.size()) > remaining_budget) {
            snippet = snippet.substr(0, static_cast<std::size_t>(remaining_budget));
            item.truncated = true;
            warnings.push_back("Snippet truncated by budget: " + item.symbol.qualified_name);
        }
        remaining_budget -= static_cast<int>(snippet.size());
        item.code = std::move(snippet);
    } else {
        item.truncated = true;
        warnings.push_back("Snippet omitted by budget: " + item.symbol.qualified_name);
    }
}

bool is_identifier_character(char value)
{
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_' || value == '$';
}

std::string symbol_tail_name(const std::string& qualified_name)
{
    const auto dot = qualified_name.find_last_of(".:\\/");
    return dot == std::string::npos ? qualified_name : qualified_name.substr(dot + 1);
}

bool text_contains_symbol_reference(const std::string& text, const repolens::ContextSymbolCandidate& symbol)
{
    std::vector<std::string> names{symbol.name};
    const auto tail = symbol_tail_name(symbol.qualified_name);
    if (!tail.empty() && tail != symbol.name) {
        names.push_back(tail);
    }

    for (const auto& name : names) {
        if (name.size() < 3) {
            continue;
        }
        std::size_t position = text.find(name);
        while (position != std::string::npos) {
            const bool left_ok = position == 0 || !is_identifier_character(text[position - 1]);
            const auto right_index = position + name.size();
            const bool right_ok = right_index >= text.size() || !is_identifier_character(text[right_index]);
            if (left_ok && right_ok) {
                return true;
            }
            position = text.find(name, position + 1);
        }
    }

    return false;
}

bool is_context_expandable_symbol(const repolens::ContextSymbolCandidate& symbol)
{
    const auto kind = symbol.kind;
    return kind.find("method") != std::string::npos ||
        kind.find("function") != std::string::npos ||
        kind.find("constructor") != std::string::npos ||
        kind.find("property") != std::string::npos ||
        kind.find("attribute") != std::string::npos ||
        kind.find("field") != std::string::npos ||
        kind.find("class") != std::string::npos ||
        kind.find("struct") != std::string::npos ||
        kind.find("interface") != std::string::npos ||
        kind.find("trait") != std::string::npos ||
        kind.find("enum") != std::string::npos ||
        kind.find("type") != std::string::npos;
}

void expand_context_levels(
    std::vector<ContextItem>& items,
    const std::vector<repolens::ContextSymbolCandidate>& active_symbols,
    int requested_level,
    int& remaining_budget,
    std::unordered_set<long long>& included_symbol_ids,
    std::vector<std::string>& warnings)
{
    if (requested_level <= 0) {
        return;
    }

    for (int level = 0; level < requested_level; ++level) {
        const auto existing_count = items.size();
        for (std::size_t item_index = 0; item_index < existing_count; ++item_index) {
            const auto source_level = items[item_index].level;
            const auto source_code = items[item_index].code;
            const auto source_symbol_id = items[item_index].symbol.symbol_id;
            const auto source_qualified_name = items[item_index].symbol.qualified_name;
            if (source_level != level || source_code.empty()) {
                continue;
            }

            for (const auto& candidate : active_symbols) {
                if (candidate.symbol_id == source_symbol_id || included_symbol_ids.count(candidate.symbol_id) != 0) {
                    continue;
                }
                if (!is_context_expandable_symbol(candidate) || !text_contains_symbol_reference(source_code, candidate)) {
                    continue;
                }

                ContextItem item;
                item.requested_symbol = candidate.name;
                item.symbol = candidate;
                item.related = true;
                item.relation_type = "referenced_by_level";
                item.source_qualified_name = source_qualified_name;
                item.level = level + 1;
                included_symbol_ids.insert(candidate.symbol_id);
                fill_context_code(item, remaining_budget, warnings);
                items.push_back(std::move(item));
            }
        }
    }
}

bool symbol_matches_signature(const repolens::CodeSymbol& symbol, const std::string& query)
{
    return symbol.name == query ||
        symbol.qualified_name == query ||
        symbol.signature == query ||
        (!symbol.signature.empty() && symbol.signature.find(query) != std::string::npos) ||
        (!symbol.qualified_name.empty() && symbol.qualified_name.find(query) != std::string::npos);
}


struct QualityOptions {
    bool dead_code = false;
    bool complexity = false;
    bool unresolved = false;
    bool json = false;
    int max_function_lines = 200;
    int max_file_lines = 1000;
    int complexity_threshold = 10;
    int top = 30;
};

struct QualityComplexityFinding {
    repolens::FactSymbol symbol;
    int lines = 0;
    int complexity = 1;
};

struct QualityFileFinding {
    repolens::IndexedFileFact file;
};

struct QualitySymbolFinding {
    repolens::FactSymbol symbol;
    int lines = 0;
};

struct QualityCouplingFinding {
    repolens::FactSymbol symbol;
    int fan_in = 0;
    int fan_out = 0;
};

struct QualityResolutionFinding {
    repolens::ArchitectureEdgeFact edge;
};

struct QualityReport {
    QualityOptions options;
    std::vector<repolens::IndexedFileFact> files;
    std::vector<repolens::FactSymbol> symbols;
    std::vector<repolens::SymbolReferenceFact> unresolved_references;
    std::vector<QualitySymbolFinding> dead_code_candidates;
    std::vector<QualityComplexityFinding> complex_symbols;
    std::vector<QualityFileFinding> large_files;
    std::vector<QualitySymbolFinding> large_symbols;
    std::vector<QualityCouplingFinding> highly_referenced_symbols;
    std::vector<QualityCouplingFinding> high_coupling_symbols;
    std::vector<QualityResolutionFinding> weak_relationships;
};

bool quality_is_function_like(const std::string& kind)
{
    const auto lower = ascii_lower(kind);
    return lower.find("function") != std::string::npos ||
        lower.find("method") != std::string::npos ||
        lower.find("constructor") != std::string::npos ||
        lower.find("destructor") != std::string::npos ||
        lower.find("procedure") != std::string::npos;
}

bool quality_is_dead_code_candidate_kind(const std::string& kind)
{
    const auto lower = ascii_lower(kind);
    return quality_is_function_like(kind) ||
        lower.find("class") != std::string::npos ||
        lower.find("struct") != std::string::npos ||
        lower.find("interface") != std::string::npos ||
        lower.find("enum") != std::string::npos;
}

std::string quality_symbol_name(const repolens::FactSymbol& symbol)
{
    return symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
}

bool quality_is_obvious_entry_point(const repolens::FactSymbol& symbol)
{
    const auto name = symbol.name.empty() ? symbol.qualified_name : symbol.name;
    return name == "main" || name == "Main" || name == "WinMain" || name == "wWinMain" ||
        name == "Program" || name == "Startup" || name == "init" || name == "Init";
}

bool quality_is_external_public_api(const repolens::FactSymbol& symbol)
{
    const auto visibility = ascii_lower(symbol.visibility);
    const auto kind = ascii_lower(symbol.kind);
    return (visibility == "public" || visibility == "protected") &&
        (kind.find("class") != std::string::npos || kind.find("interface") != std::string::npos || kind.find("struct") != std::string::npos);
}

bool quality_identifier_boundary(const std::string& text, std::size_t position, std::size_t length)
{
    const bool left_ok = position == 0 || !is_identifier_character(text[position - 1]);
    const auto right_index = position + length;
    const bool right_ok = right_index >= text.size() || !is_identifier_character(text[right_index]);
    return left_ok && right_ok;
}

int quality_count_keyword(const std::string& text, const std::string& keyword)
{
    int count = 0;
    std::size_t position = text.find(keyword);
    while (position != std::string::npos) {
        if (quality_identifier_boundary(text, position, keyword.size())) {
            ++count;
        }
        position = text.find(keyword, position + keyword.size());
    }
    return count;
}

int quality_count_substring(const std::string& text, const std::string& needle)
{
    int count = 0;
    std::size_t position = text.find(needle);
    while (position != std::string::npos) {
        ++count;
        position = text.find(needle, position + needle.size());
    }
    return count;
}

int quality_cyclomatic_complexity(const std::string& source)
{
    int complexity = 1;
    const std::vector<std::string> keywords{"if", "for", "while", "case", "catch", "except", "elif", "elseif", "when"};
    for (const auto& keyword : keywords) {
        complexity += quality_count_keyword(source, keyword);
    }
    complexity += quality_count_substring(source, "&&");
    complexity += quality_count_substring(source, "||");
    complexity += quality_count_substring(source, "?");
    return complexity;
}

QualityOptions read_quality_options(int argc, char* argv[])
{
    QualityOptions options;
    options.dead_code = has_flag(argc, argv, "--dead-code");
    options.complexity = has_flag(argc, argv, "--complexity");
    options.unresolved = has_flag(argc, argv, "--unresolved");
    if (!options.dead_code && !options.complexity && !options.unresolved) {
        options.dead_code = true;
        options.complexity = true;
        options.unresolved = true;
    }
    options.json = fact_output_format(argc, argv) == "json";
    options.max_function_lines = std::max(1, read_option_int(argc, argv, "--max-function-lines", 200));
    options.max_file_lines = std::max(1, read_option_int(argc, argv, "--max-file-lines", 1000));
    options.complexity_threshold = std::max(1, read_option_int(argc, argv, "--complexity-threshold", 10));
    options.top = std::max(1, read_option_int(argc, argv, "--top", 30));
    return options;
}

void quality_limit(std::vector<QualitySymbolFinding>& values, int top)
{
    if (static_cast<int>(values.size()) > top) {
        values.resize(static_cast<std::size_t>(top));
    }
}

void quality_limit(std::vector<QualityComplexityFinding>& values, int top)
{
    if (static_cast<int>(values.size()) > top) {
        values.resize(static_cast<std::size_t>(top));
    }
}

void quality_limit(std::vector<QualityFileFinding>& values, int top)
{
    if (static_cast<int>(values.size()) > top) {
        values.resize(static_cast<std::size_t>(top));
    }
}

void quality_limit(std::vector<QualityCouplingFinding>& values, int top)
{
    if (static_cast<int>(values.size()) > top) {
        values.resize(static_cast<std::size_t>(top));
    }
}

void quality_limit(std::vector<QualityResolutionFinding>& values, int top)
{
    if (static_cast<int>(values.size()) > top) {
        values.resize(static_cast<std::size_t>(top));
    }
}

QualityReport build_quality_report(repolens::SqliteDatabase& database, long long repository_id, const QualityOptions& options)
{
    QualityReport report;
    report.options = options;
    report.files = database.active_files(repository_id);
    report.unresolved_references = database.unresolved_references(repository_id);

    for (const auto& file : report.files) {
        auto file_symbols = database.symbols_for_file(repository_id, file.relative_path);
        report.symbols.insert(report.symbols.end(), file_symbols.begin(), file_symbols.end());
        if (file.line_count > options.max_file_lines) {
            QualityFileFinding finding;
            finding.file = file;
            report.large_files.push_back(finding);
        }
    }

    const auto edges = database.architecture_edges(repository_id);
    std::unordered_map<long long, int> fan_in;
    std::unordered_map<long long, int> fan_out;
    std::unordered_map<long long, repolens::FactSymbol> symbol_by_id;
    for (const auto& symbol : report.symbols) {
        symbol_by_id[symbol.row_id] = symbol;
    }

    for (const auto& edge : edges) {
        if (edge.source_symbol_id != 0) {
            ++fan_out[edge.source_symbol_id];
        }
        if (edge.target_symbol_id != 0) {
            ++fan_in[edge.target_symbol_id];
        }
        const auto strategy = ascii_lower(edge.resolution_strategy);
        if (edge.confidence < 0.9 || strategy.find("heuristic") != std::string::npos || strategy.find("unresolved") != std::string::npos) {
            QualityResolutionFinding finding;
            finding.edge = edge;
            report.weak_relationships.push_back(finding);
        }
    }

    for (const auto& symbol : report.symbols) {
        const int lines = std::max(0, symbol.line_end - symbol.line_start + 1);
        const int incoming = fan_in[symbol.row_id];
        const int outgoing = fan_out[symbol.row_id];

        if (quality_is_function_like(symbol.kind) && lines > options.max_function_lines) {
            report.large_symbols.push_back(QualitySymbolFinding{symbol, lines});
        }

        if (quality_is_function_like(symbol.kind)) {
            try {
                const auto source = read_line_range(symbol.absolute_path, symbol.line_start, symbol.line_end);
                const int complexity = quality_cyclomatic_complexity(source);
                if (complexity >= options.complexity_threshold) {
                    QualityComplexityFinding finding;
                    finding.symbol = symbol;
                    finding.lines = lines;
                    finding.complexity = complexity;
                    report.complex_symbols.push_back(finding);
                }
            } catch (const std::exception&) {
            }
        }

        if (incoming == 0 && quality_is_dead_code_candidate_kind(symbol.kind) &&
            !quality_is_obvious_entry_point(symbol) && !quality_is_external_public_api(symbol)) {
            report.dead_code_candidates.push_back(QualitySymbolFinding{symbol, lines});
        }

        if (incoming > 0) {
            report.highly_referenced_symbols.push_back(QualityCouplingFinding{symbol, incoming, outgoing});
        }
        if (incoming + outgoing > 0) {
            report.high_coupling_symbols.push_back(QualityCouplingFinding{symbol, incoming, outgoing});
        }
    }

    std::sort(report.large_files.begin(), report.large_files.end(), [](const auto& left, const auto& right) {
        if (left.file.line_count != right.file.line_count) return left.file.line_count > right.file.line_count;
        return left.file.relative_path < right.file.relative_path;
    });
    std::sort(report.large_symbols.begin(), report.large_symbols.end(), [](const auto& left, const auto& right) {
        if (left.lines != right.lines) return left.lines > right.lines;
        if (left.symbol.file_path != right.symbol.file_path) return left.symbol.file_path < right.symbol.file_path;
        return left.symbol.line_start < right.symbol.line_start;
    });
    std::sort(report.complex_symbols.begin(), report.complex_symbols.end(), [](const auto& left, const auto& right) {
        if (left.complexity != right.complexity) return left.complexity > right.complexity;
        if (left.lines != right.lines) return left.lines > right.lines;
        return quality_symbol_name(left.symbol) < quality_symbol_name(right.symbol);
    });
    std::sort(report.dead_code_candidates.begin(), report.dead_code_candidates.end(), [](const auto& left, const auto& right) {
        if (left.symbol.file_path != right.symbol.file_path) return left.symbol.file_path < right.symbol.file_path;
        if (left.symbol.line_start != right.symbol.line_start) return left.symbol.line_start < right.symbol.line_start;
        return quality_symbol_name(left.symbol) < quality_symbol_name(right.symbol);
    });
    std::sort(report.highly_referenced_symbols.begin(), report.highly_referenced_symbols.end(), [](const auto& left, const auto& right) {
        if (left.fan_in != right.fan_in) return left.fan_in > right.fan_in;
        if (left.symbol.file_path != right.symbol.file_path) return left.symbol.file_path < right.symbol.file_path;
        return quality_symbol_name(left.symbol) < quality_symbol_name(right.symbol);
    });
    std::sort(report.high_coupling_symbols.begin(), report.high_coupling_symbols.end(), [](const auto& left, const auto& right) {
        const int left_total = left.fan_in + left.fan_out;
        const int right_total = right.fan_in + right.fan_out;
        if (left_total != right_total) return left_total > right_total;
        if (left.fan_out != right.fan_out) return left.fan_out > right.fan_out;
        return quality_symbol_name(left.symbol) < quality_symbol_name(right.symbol);
    });
    std::sort(report.weak_relationships.begin(), report.weak_relationships.end(), [](const auto& left, const auto& right) {
        if (left.edge.confidence != right.edge.confidence) return left.edge.confidence < right.edge.confidence;
        if (left.edge.source_file != right.edge.source_file) return left.edge.source_file < right.edge.source_file;
        return left.edge.relationship_id < right.edge.relationship_id;
    });

    quality_limit(report.large_files, options.top);
    quality_limit(report.large_symbols, options.top);
    quality_limit(report.complex_symbols, options.top);
    quality_limit(report.dead_code_candidates, options.top);
    quality_limit(report.highly_referenced_symbols, options.top);
    quality_limit(report.high_coupling_symbols, options.top);
    quality_limit(report.weak_relationships, options.top);
    if (static_cast<int>(report.unresolved_references.size()) > options.top) {
        report.unresolved_references.resize(static_cast<std::size_t>(options.top));
    }
    return report;
}

std::string quality_symbol_json(const repolens::FactSymbol& symbol, const std::string& indent)
{
    std::ostringstream output;
    output << indent << "{\"id\": " << symbol.row_id
           << ", \"kind\": \"" << json_escape(symbol.kind)
           << "\", \"name\": \"" << json_escape(symbol.name)
           << "\", \"qualified_name\": \"" << json_escape(symbol.qualified_name)
           << "\", \"file\": \"" << json_escape(symbol.file_path)
           << "\", \"line_start\": " << symbol.line_start
           << ", \"line_end\": " << symbol.line_end << "}";
    return output.str();
}

std::string quality_json(const QualityReport& report)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"summary\": {\n";
    output << "    \"files\": " << report.files.size() << ",\n";
    output << "    \"symbols\": " << report.symbols.size() << ",\n";
    output << "    \"unresolved_references\": " << report.unresolved_references.size() << ",\n";
    output << "    \"dead_code_candidates\": " << report.dead_code_candidates.size() << ",\n";
    output << "    \"complex_symbols\": " << report.complex_symbols.size() << ",\n";
    output << "    \"large_files\": " << report.large_files.size() << ",\n";
    output << "    \"large_symbols\": " << report.large_symbols.size() << ",\n";
    output << "    \"weak_relationships\": " << report.weak_relationships.size() << "\n";
    output << "  },\n";
    output << "  \"thresholds\": {\"max_function_lines\": " << report.options.max_function_lines
           << ", \"max_file_lines\": " << report.options.max_file_lines
           << ", \"complexity_threshold\": " << report.options.complexity_threshold
           << ", \"top\": " << report.options.top << "},\n";

    output << "  \"unresolved_references\": [\n";
    for (std::size_t index = 0; index < report.unresolved_references.size(); ++index) {
        output << symbol_reference_json_object(report.unresolved_references[index], "    ");
        if (index + 1 < report.unresolved_references.size()) output << ',';
        output << '\n';
    }
    output << "  ],\n";

    auto write_symbol_findings = [&](const char* name, const std::vector<QualitySymbolFinding>& values) {
        output << "  \"" << name << "\": [\n";
        for (std::size_t index = 0; index < values.size(); ++index) {
            output << "    {\"lines\": " << values[index].lines << ", \"symbol\": " << quality_symbol_json(values[index].symbol, "") << "}";
            if (index + 1 < values.size()) output << ',';
            output << '\n';
        }
        output << "  ],\n";
    };
    write_symbol_findings("dead_code_candidates", report.dead_code_candidates);
    write_symbol_findings("large_symbols", report.large_symbols);

    output << "  \"complex_symbols\": [\n";
    for (std::size_t index = 0; index < report.complex_symbols.size(); ++index) {
        output << "    {\"complexity\": " << report.complex_symbols[index].complexity
               << ", \"lines\": " << report.complex_symbols[index].lines
               << ", \"symbol\": " << quality_symbol_json(report.complex_symbols[index].symbol, "") << "}";
        if (index + 1 < report.complex_symbols.size()) output << ',';
        output << '\n';
    }
    output << "  ],\n";

    output << "  \"large_files\": [\n";
    for (std::size_t index = 0; index < report.large_files.size(); ++index) {
        const auto& file = report.large_files[index].file;
        output << "    {\"file\": \"" << json_escape(file.relative_path) << "\", \"language\": \"" << json_escape(file.language)
               << "\", \"lines\": " << file.line_count << ", \"size_bytes\": " << file.size_bytes << "}";
        if (index + 1 < report.large_files.size()) output << ',';
        output << '\n';
    }
    output << "  ],\n";

    auto write_coupling = [&](const char* name, const std::vector<QualityCouplingFinding>& values) {
        output << "  \"" << name << "\": [\n";
        for (std::size_t index = 0; index < values.size(); ++index) {
            output << "    {\"fan_in\": " << values[index].fan_in << ", \"fan_out\": " << values[index].fan_out
                   << ", \"symbol\": " << quality_symbol_json(values[index].symbol, "") << "}";
            if (index + 1 < values.size()) output << ',';
            output << '\n';
        }
        output << "  ],\n";
    };
    write_coupling("highly_referenced_symbols", report.highly_referenced_symbols);
    write_coupling("high_coupling_symbols", report.high_coupling_symbols);

    output << "  \"weak_relationships\": [\n";
    for (std::size_t index = 0; index < report.weak_relationships.size(); ++index) {
        const auto& edge = report.weak_relationships[index].edge;
        output << "    {\"source_symbol\": \"" << json_escape(edge.source_symbol)
               << "\", \"target_symbol\": \"" << json_escape(edge.target_symbol)
               << "\", \"file\": \"" << json_escape(edge.source_file)
               << "\", \"relationship_type\": \"" << json_escape(edge.relationship_type)
               << "\", \"confidence\": " << edge.confidence
               << ", \"resolution_strategy\": \"" << json_escape(edge.resolution_strategy) << "\"}";
        if (index + 1 < report.weak_relationships.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n";
    output << "}\n";
    return output.str();
}

std::string quality_text(const QualityReport& report)
{
    std::ostringstream output;
    output << "Quality Report\n";
    output << "Files: " << report.files.size() << "\n";
    output << "Symbols: " << report.symbols.size() << "\n";
    output << "Unresolved references: " << report.unresolved_references.size() << "\n";
    output << "Dead code candidates: " << report.dead_code_candidates.size() << "\n";
    output << "Complex symbols: " << report.complex_symbols.size() << "\n";
    output << "Large files: " << report.large_files.size() << "\n";
    output << "Large symbols: " << report.large_symbols.size() << "\n";
    output << "Weak relationships: " << report.weak_relationships.size() << "\n\n";

    if (report.options.unresolved) {
        output << "Unresolved references\n";
        for (const auto& reference : report.unresolved_references) {
            output << "  " << reference.source_file << ':' << reference.line << " " << reference.reference_text
                   << " strategy=" << reference.resolution_strategy << "\n";
        }
        output << '\n';
    }
    if (report.options.dead_code) {
        output << "Dead code candidates\n";
        for (const auto& finding : report.dead_code_candidates) {
            output << "  candidate " << quality_symbol_name(finding.symbol) << " " << finding.symbol.file_path
                   << ':' << finding.symbol.line_start << '-' << finding.symbol.line_end << "\n";
        }
        output << '\n';
    }
    if (report.options.complexity) {
        output << "Complexity\n";
        for (const auto& finding : report.complex_symbols) {
            output << "  complexity=" << finding.complexity << " lines=" << finding.lines << " "
                   << quality_symbol_name(finding.symbol) << " " << finding.symbol.file_path << ':'
                   << finding.symbol.line_start << '-' << finding.symbol.line_end << "\n";
        }
        output << '\n';
    }
    output << "Large files\n";
    for (const auto& finding : report.large_files) {
        output << "  lines=" << finding.file.line_count << " " << finding.file.relative_path << "\n";
    }
    output << "\nLarge symbols\n";
    for (const auto& finding : report.large_symbols) {
        output << "  lines=" << finding.lines << " " << quality_symbol_name(finding.symbol) << " "
               << finding.symbol.file_path << ':' << finding.symbol.line_start << '-' << finding.symbol.line_end << "\n";
    }
    output << "\nHighly referenced symbols\n";
    for (const auto& finding : report.highly_referenced_symbols) {
        output << "  fan_in=" << finding.fan_in << " fan_out=" << finding.fan_out << " " << quality_symbol_name(finding.symbol) << "\n";
    }
    output << "\nHigh fan-in/fan-out symbols\n";
    for (const auto& finding : report.high_coupling_symbols) {
        output << "  fan_in=" << finding.fan_in << " fan_out=" << finding.fan_out << " " << quality_symbol_name(finding.symbol) << "\n";
    }
    output << "\nMissing or weak relationship resolution\n";
    for (const auto& finding : report.weak_relationships) {
        output << "  confidence=" << finding.edge.confidence << " strategy=" << finding.edge.resolution_strategy
               << " " << finding.edge.source_symbol << " -> " << finding.edge.target_symbol << "\n";
    }
    return output.str();
}

int run_quality(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens quality --index-dir <index_path> [--dead-code] [--complexity] [--unresolved] [--max-function-lines <n>] [--max-file-lines <n>] [--complexity-threshold <n>] [--top <n>] [--format text|json|--json]");
    }
    const auto options = read_quality_options(argc, argv);
    repolens::SqliteDatabase database{database_path_from_index(*index_dir)};
    database.create_schema();
    const auto status = read_required_status(database);
    const auto report = build_quality_report(database, status.repository_id, options);
    if (options.json) {
        std::cout << quality_json(report);
    } else {
        std::cout << quality_text(report);
    }
    return 0;
}

std::string direct_context_json(
    const std::filesystem::path& source_file,
    const std::optional<std::filesystem::path>& repo_root_option,
    const std::string& signature,
    int budget_chars)
{
    const auto absolute_file = std::filesystem::canonical(source_file);
    const auto repo_root = repo_root_option
        ? canonical_existing_directory(*repo_root_option, "repo_root")
        : std::filesystem::canonical(absolute_file.parent_path());
    if (!is_same_or_child_path(repo_root, absolute_file)) {
        throw std::runtime_error("--file must be inside --repo-root.");
    }

    const auto metadata = repolens::scan_file(repo_root, absolute_file);
    repolens::InterpreterRegistry interpreters;
    register_all_interpreters(interpreters);
    const auto* interpreter = interpreters.find_for_file(metadata);
    if (!interpreter) {
        throw std::runtime_error("No parser is registered for file: " + absolute_file.string());
    }

    const auto parse_result = interpreter->parse_file(metadata);
    std::vector<ContextItem> items;
    std::vector<std::string> warnings = parse_result.diagnostics;
    int remaining_budget = std::max(0, budget_chars);
    long long symbol_id = 1;

    for (const auto& symbol : parse_result.symbols) {
        if (!symbol_matches_signature(symbol, signature)) {
            continue;
        }

        ContextItem item;
        item.requested_symbol = signature;
        item.symbol.symbol_id = symbol_id++;
        item.symbol.kind = symbol.kind;
        item.symbol.name = symbol.name;
        item.symbol.qualified_name = symbol.qualified_name;
        item.symbol.signature = symbol.signature;
        item.symbol.relative_path = metadata.relative_path;
        item.symbol.absolute_path = metadata.absolute_path;
        item.symbol.line_start = symbol.line_start;
        item.symbol.line_end = symbol.line_end;
        fill_context_code(item, remaining_budget, warnings);
        items.push_back(std::move(item));
    }

    if (items.empty()) {
        warnings.push_back("Signature not found in file: " + signature);
    }

    std::ostringstream output;
    output << "{\n";
    output << "  \"repository\": {\n";
    output << "    \"repo_root\": \"" << json_escape(repo_root.string()) << "\",\n";
    output << "    \"index_root\": \"\",\n";
    output << "    \"last_indexed_at\": \"realtime\"\n";
    output << "  },\n";
    output << "  \"query\": {\n";
    output << "    \"symbols\": " << string_array_json(std::vector<std::string>{signature}, "      ") << ",\n";
    output << "    \"partial\": true\n";
    output << "  },\n";
    output << "  \"budget\": {\n";
    output << "    \"requested_chars\": " << std::max(0, budget_chars) << ",\n";
    output << "    \"used_chars\": " << (std::max(0, budget_chars) - remaining_budget) << ",\n";
    output << "    \"remaining_chars\": " << remaining_budget << "\n";
    output << "  },\n";
    output << "  \"symbols\": [\n";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        output << "    {\n";
        output << "      \"requested_symbol\": \"" << json_escape(item.requested_symbol) << "\",\n";
        output << "      \"level\": 0,\n";
        output << "      \"kind\": \"" << json_escape(item.symbol.kind) << "\",\n";
        output << "      \"name\": \"" << json_escape(item.symbol.name) << "\",\n";
        output << "      \"qualified_name\": \"" << json_escape(item.symbol.qualified_name) << "\",\n";
        output << "      \"signature\": \"" << json_escape(item.symbol.signature) << "\",\n";
        output << "      \"file\": \"" << json_escape(item.symbol.relative_path) << "\",\n";
        output << "      \"line_start\": " << item.symbol.line_start << ",\n";
        output << "      \"line_end\": " << item.symbol.line_end << ",\n";
        output << "      \"related\": false,\n";
        output << "      \"relation_type\": \"\",\n";
        output << "      \"source_qualified_name\": \"\",\n";
        output << "      \"truncated\": " << (item.truncated ? "true" : "false") << ",\n";
        output << "      \"code\": \"" << json_escape(item.code) << "\"\n";
        output << "    }";
        if (index + 1 < items.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n";
    output << "  \"reduced_file_tree\": [],\n";
    output << "  \"warnings\": " << string_array_json(warnings, "    ") << "\n";
    output << "}\n";
    return output.str();
}

int run_direct_context(int argc, char* argv[])
{
    const auto source_file = read_option_path(argc, argv, "--file");
    const auto signature = read_option_string(argc, argv, "--signature");
    const auto format = read_option_string(argc, argv, "--format").value_or("json");
    if (!source_file || !signature || signature->empty()) {
        throw std::runtime_error("Usage: repolens direct-context --file <path> --signature <text> [--repo-root <path>] [--budget-chars <n>] --format json");
    }
    if (format != "json") {
        throw std::runtime_error("direct-context supports --format json only.");
    }

    std::cout << direct_context_json(
        *source_file,
        read_option_path(argc, argv, "--repo-root"),
        *signature,
        read_option_int(argc, argv, "--budget-chars", 60000));

    return 0;
}

std::string context_basic_json(
    const std::filesystem::path& index_dir,
    const std::vector<std::string>& requested_symbols,
    bool partial_match,
    int requested_level = 0,
    bool include_situated = false)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();

    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    std::vector<ContextItem> items;
    std::vector<std::string> warnings;
    std::unordered_set<long long> included_symbol_ids;

    for (const auto& requested_symbol : requested_symbols) {
        const auto candidates = database.find_context_symbols(status->repository_id, requested_symbol, partial_match);
        if (candidates.empty()) {
            warnings.push_back(std::string{partial_match ? "Symbol not found by partial match: " : "Symbol not found: "} + requested_symbol);
            continue;
        }

        if (candidates.size() > 1) {
            std::string warning = std::string{partial_match ? "Ambiguous partial symbol: " : "Ambiguous symbol: "} + requested_symbol + " matched ";
            warning += std::to_string(candidates.size()) + " candidates";
            warnings.push_back(warning);
        }

        for (const auto& candidate : candidates) {
            ContextItem item;
            item.requested_symbol = requested_symbol;
            item.symbol = candidate;
            item.level = 0;
            item.code = read_line_range(candidate.absolute_path, candidate.line_start, candidate.line_end);
            items.push_back(std::move(item));
            included_symbol_ids.insert(candidate.symbol_id);
        }
    }

    if (requested_level > 0) {
        int basic_budget = 2147483647;
        const auto active_symbols = database.active_context_symbols(status->repository_id);
        expand_context_levels(items, active_symbols, requested_level, basic_budget, included_symbol_ids, warnings);
    }
    if (include_situated) {
        int situated_budget = 2147483647;
        apply_situated_context(database, status->repository_id, items, situated_budget, warnings);
    }

    std::ostringstream output;
    output << "{\n";
    output << "  \"symbols\": [\n";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        output << "    {\n";
        output << "      \"level\": " << item.level << ",\n";
        output << "      \"file\": \"" << json_escape(item.symbol.relative_path) << "\",\n";
        output << "      \"line_start\": " << item.symbol.line_start << ",\n";
        output << "      \"line_end\": " << item.symbol.line_end << ",\n";
        if (include_situated) {
            output << "      \"situated_description\": \"" << json_escape(item.situated_description) << "\",\n";
        }
        output << "      \"code\": \"" << json_escape(item.code) << "\"\n";
        output << "    }";
        if (index + 1 < items.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n";
    output << "  \"warnings\": " << string_array_json(warnings, "    ") << "\n";
    output << "}\n";
    return output.str();
}

std::string virtual_file_search_json_array(const std::vector<repolens::VirtualFileSearchResult>& results, const std::string& indent)
{
    std::ostringstream output;
    output << "[";
    if (!results.empty()) {
        output << "\n";
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        output << indent << "  {\n"
               << indent << "    \"id\": " << result.file.id << ",\n"
               << indent << "    \"type\": \"" << json_escape(result.file.signal_type) << "\",\n"
               << indent << "    \"virtual_path\": \"" << json_escape(result.file.virtual_path) << "\",\n"
               << indent << "    \"source_path\": \"" << json_escape(result.file.source_path) << "\",\n"
               << indent << "    \"imported_at\": \"" << json_escape(result.file.imported_at) << "\",\n"
               << indent << "    \"content_hash\": \"" << json_escape(result.file.content_hash) << "\",\n"
               << indent << "    \"size_bytes\": " << result.file.size_bytes << ",\n"
               << indent << "    \"line_count\": " << result.file.line_count << ",\n"
               << indent << "    \"truncated\": " << (result.file.truncated ? "true" : "false") << ",\n"
               << indent << "    \"line\": " << result.line << ",\n"
               << indent << "    \"snippet\": \"" << json_escape(result.snippet) << "\"\n"
               << indent << "  }";
        if (index + 1 < results.size()) {
            output << ',';
        }
        output << "\n";
    }
    if (!results.empty()) {
        output << indent;
    }
    output << "]";
    return output.str();
}
std::string context_json(
    const std::filesystem::path& index_dir,
    const std::vector<std::string>& requested_symbols,
    int budget_chars,
    bool include_tree,
    bool include_types,
    bool partial_match,
    int requested_level = 0,
    bool include_situated = false,
    const std::string& signal_query = "")
{
    budget_chars = std::max(0, budget_chars);
    int remaining_budget = budget_chars;

    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();

    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    std::vector<ContextItem> items;
    std::vector<std::string> warnings;
    std::vector<long long> primary_symbol_ids;
    std::unordered_set<long long> included_symbol_ids;

    for (const auto& requested_symbol : requested_symbols) {
        const auto candidates = database.find_context_symbols(status->repository_id, requested_symbol, partial_match);
        if (candidates.empty()) {
            warnings.push_back(std::string{partial_match ? "Symbol not found by partial match: " : "Symbol not found: "} + requested_symbol);
            continue;
        }

        if (candidates.size() > 1) {
            std::string warning = std::string{partial_match ? "Ambiguous partial symbol: " : "Ambiguous symbol: "} + requested_symbol + " matched ";
            warning += std::to_string(candidates.size()) + " candidates";
            warnings.push_back(warning);
        }

        for (const auto& candidate : candidates) {
            ContextItem item;
            item.requested_symbol = requested_symbol;
            item.symbol = candidate;
            item.level = 0;
            primary_symbol_ids.push_back(candidate.symbol_id);
            included_symbol_ids.insert(candidate.symbol_id);

            fill_context_code(item, remaining_budget, warnings);

            items.push_back(std::move(item));
        }
    }

    if (include_types) {
        for (const auto& related_symbol : database.find_related_type_symbols(status->repository_id, primary_symbol_ids)) {
            if (included_symbol_ids.find(related_symbol.symbol.symbol_id) != included_symbol_ids.end()) {
                continue;
            }

            ContextItem item;
            item.requested_symbol = related_symbol.symbol.name;
            item.symbol = related_symbol.symbol;
            item.related = true;
            item.relation_type = related_symbol.relation_type;
            item.source_qualified_name = related_symbol.source_qualified_name;
            item.level = 0;
            included_symbol_ids.insert(related_symbol.symbol.symbol_id);
            fill_context_code(item, remaining_budget, warnings);
            items.push_back(std::move(item));
        }
    }

    if (requested_level > 0) {
        const auto active_symbols = database.active_context_symbols(status->repository_id);
        expand_context_levels(items, active_symbols, requested_level, remaining_budget, included_symbol_ids, warnings);
    }
    if (include_situated) {
        apply_situated_context(database, status->repository_id, items, remaining_budget, warnings);
    }

    const auto tree_paths = include_tree ? database.active_file_paths(status->repository_id) : std::vector<std::string>{};
    const auto signal_results = signal_query.empty() ? std::vector<repolens::VirtualFileSearchResult>{} : database.search_virtual_files(status->repository_id, signal_query, 10);

    std::ostringstream output;
    output << "{\n";
    output << "  \"repository\": {\n";
    output << "    \"repo_root\": \"" << json_escape(status->repo_root) << "\",\n";
    output << "    \"index_root\": \"" << json_escape(status->index_root) << "\",\n";
    output << "    \"last_indexed_at\": \"" << json_escape(status->last_indexed_at) << "\"\n";
    output << "  },\n";
    output << "  \"query\": {\n";
    output << "    \"symbols\": " << string_array_json(requested_symbols, "      ") << ",\n";
    output << "    \"partial\": " << (partial_match ? "true" : "false") << "\n";
    output << "  },\n";
    output << "  \"budget\": {\n";
    output << "    \"requested_chars\": " << budget_chars << ",\n";
    output << "    \"used_chars\": " << (budget_chars - remaining_budget) << ",\n";
    output << "    \"remaining_chars\": " << remaining_budget << "\n";
    output << "  },\n";
    output << "  \"symbols\": [\n";
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        output << "    {\n";
        output << "      \"requested_symbol\": \"" << json_escape(item.requested_symbol) << "\",\n";
        output << "      \"level\": " << item.level << ",\n";
        output << "      \"kind\": \"" << json_escape(item.symbol.kind) << "\",\n";
        output << "      \"name\": \"" << json_escape(item.symbol.name) << "\",\n";
        output << "      \"qualified_name\": \"" << json_escape(item.symbol.qualified_name) << "\",\n";
        output << "      \"signature\": \"" << json_escape(item.symbol.signature) << "\",\n";
        output << "      \"file\": \"" << json_escape(item.symbol.relative_path) << "\",\n";
        output << "      \"line_start\": " << item.symbol.line_start << ",\n";
        output << "      \"line_end\": " << item.symbol.line_end << ",\n";
        output << "      \"related\": " << (item.related ? "true" : "false") << ",\n";
        output << "      \"relation_type\": \"" << json_escape(item.relation_type) << "\",\n";
        output << "      \"source_qualified_name\": \"" << json_escape(item.source_qualified_name) << "\",\n";
        output << "      \"truncated\": " << (item.truncated ? "true" : "false") << ",\n";
        if (include_situated) {
            output << "      \"situated_description\": \"" << json_escape(item.situated_description) << "\",\n";
        }
        output << "      \"code\": \"" << json_escape(item.code) << "\"\n";
        output << "    }";
        if (index + 1 < items.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n";
    output << "  \"reduced_file_tree\": " << string_array_json(tree_paths, "    ") << ",\n";
    output << "  \"signals\": " << virtual_file_search_json_array(signal_results, "    ") << ",\n";
    output << "  \"warnings\": " << string_array_json(warnings, "    ") << "\n";
    output << "}\n";

    return output.str();
}

void grow_index_for_context(
    const std::filesystem::path& index_dir,
    const std::vector<std::string>& requested_symbols,
    bool partial_match,
    const std::vector<std::filesystem::path>& grow_files,
    bool lite_mode)
{
    if (grow_files.empty()) {
        throw std::runtime_error("--grow requires --grow-files <a,b> so growth stays bounded and fast.");
    }

    std::filesystem::path repo_root;
    bool needs_growth = false;
    {
        const auto database_path = database_path_from_index(index_dir);
        repolens::SqliteDatabase database{database_path};
        database.create_schema();
        const auto status = database.read_repository_status();
        if (!status) {
            throw std::runtime_error("Repository metadata was not found. Run init first.");
        }

        repo_root = std::filesystem::path{status->repo_root};
        for (const auto& requested_symbol : requested_symbols) {
            if (database.find_context_symbols(status->repository_id, requested_symbol, partial_match).empty()) {
                needs_growth = true;
                break;
            }
        }
    }

    if (!needs_growth) {
        return;
    }

    NullProgressReporter reporter;
    update_selected_files(index_dir, repo_root, grow_files, lite_mode, false, reporter);
}

std::optional<std::string> positional_after_command(int argc, char* argv[])
{
    const std::set<std::string> options_with_values{
        "--index-dir", "--format", "--level", "--budget-chars", "--file", "--symbol",
        "--symbols", "--kind", "--limit", "--start", "--end", "--direction", "--depth",
        "--min-confidence", "--max-results", "--type", "--top", "--seed", "--repo-root",
        "--signature", "--grow-files", "--scip-index"
    };
    for (int index = 2; index < argc; ++index) {
        const std::string arg{argv[index]};
        if (arg.rfind("--", 0) == 0) {
            if (options_with_values.find(arg) != options_with_values.end() && index + 1 < argc) {
                ++index;
            }
            continue;
        }
        return arg;
    }
    return std::nullopt;
}

int run_context(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens context --index-dir <index_path> --symbols <A,B> [--partial] [--basic] [--level <n>] [--budget-chars <n>] [--include-tree] [--include-types] [--compact --file <path>|--symbol <name>] --format json");
    }

    if (has_flag(argc, argv, "--compact")) {
        const auto options = read_compact_options(argc, argv);
        std::vector<CompactView> views;
        const auto file_path = read_option_string(argc, argv, "--file");
        const auto symbol_option = read_option_string(argc, argv, "--symbol");
        const auto symbols_option = read_option_string(argc, argv, "--symbols");
        if (file_path) {
            views.push_back(load_compact_view_for_file(*index_dir, *file_path, options));
        } else if (symbol_option && !symbol_option->empty()) {
            views = compact_views_for_symbol(*index_dir, *symbol_option, options);
        } else if (symbols_option && !symbols_option->empty()) {
            for (const auto& symbol : split_symbols(*symbols_option)) {
                auto symbol_views = compact_views_for_symbol(*index_dir, symbol, options);
                views.insert(views.end(), symbol_views.begin(), symbol_views.end());
            }
        } else {
            throw std::runtime_error("Usage: repolens context --index-dir <index_path> --compact --file <path>|--symbol <name>|--symbols <A,B> [--json]");
        }

        if (options.json) {
            std::cout << compact_view_json(views, options);
        } else {
            std::cout << compact_view_text(views, options);
        }
        return 0;
    }

    auto symbols_option = read_option_string(argc, argv, "--symbols");
    const auto format = read_option_string(argc, argv, "--format").value_or("json");
    const auto signal_query = read_option_string(argc, argv, "--signals").value_or("");
    if (!symbols_option || symbols_option->empty()) {
        symbols_option = positional_after_command(argc, argv);
    }
    if ((!symbols_option || symbols_option->empty()) && signal_query.empty()) {
        throw std::runtime_error("Usage: repolens context --index-dir <index_path> [<symbol>|--symbols <A,B>] [--signals <query>] [--situated] [--partial] [--basic] [--level <n>] [--budget-chars <n>] [--include-tree] [--include-types] --format json");
    }
    if (format != "json") {
        throw std::runtime_error("Phase 6 context output supports --format json only.");
    }

    const bool partial_match = has_flag(argc, argv, "--partial");
    const int requested_level = std::max(0, read_option_int(argc, argv, "--level", 0));
    const auto requested_symbols = (symbols_option && !symbols_option->empty()) ? split_symbols(*symbols_option) : std::vector<std::string>{};
    if (has_flag(argc, argv, "--grow")) {
        std::vector<std::filesystem::path> grow_files;
        const auto grow_files_option = read_option_string(argc, argv, "--grow-files");
        if (grow_files_option) {
            grow_files = split_path_arguments(*grow_files_option);
        }
        grow_index_for_context(*index_dir, requested_symbols, partial_match, grow_files, has_flag(argc, argv, "--lite"));
    }

    if (has_flag(argc, argv, "--basic")) {
        std::cout << context_basic_json(*index_dir, requested_symbols, partial_match, requested_level, has_flag(argc, argv, "--situated"));
        return 0;
    }

    std::cout << context_json(
        *index_dir,
        requested_symbols,
        read_option_int(argc, argv, "--budget-chars", 60000),
        has_flag(argc, argv, "--include-tree"),
        has_flag(argc, argv, "--include-types"),
        partial_match,
        requested_level,
        has_flag(argc, argv, "--situated"),
        signal_query);

    return 0;
}
std::string update_json(const UpdateSummary& summary)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"repository\": {\n";
    output << "    \"repo_root\": \"" << json_escape(summary.repo_root) << "\",\n";
    output << "    \"index_root\": \"" << json_escape(summary.index_root) << "\",\n";
    output << "    \"database\": \"" << json_escape(summary.database_path) << "\"\n";
    output << "  },\n";
    output << "  \"mode\": \"" << (summary.lite_mode ? "lite" : "full") << "\",\n";
    output << "  \"timing\": {\n";
    output << "    \"started_at\": \"" << json_escape(summary.started_at) << "\",\n";
    output << "    \"finished_at\": \"" << json_escape(summary.finished_at) << "\",\n";
    output << "    \"elapsed_seconds\": " << std::fixed << std::setprecision(3) << summary.elapsed_seconds << "\n";
    output << "  },\n";
    output << "  \"files\": {\n";
    output << "    \"discovered\": " << summary.discovered << ",\n";
    output << "    \"ignored\": " << summary.ignored << ",\n";
    output << "    \"processed\": " << summary.processed << ",\n";
    output << "    \"scanned\": " << summary.files_scanned << ",\n";
    output << "    \"reindexed\": " << summary.files_reindexed << ",\n";
    output << "    \"source_bytes\": " << summary.source_bytes << ",\n";
    output << "    \"added\": " << summary.added << ",\n";
    output << "    \"modified\": " << summary.modified << ",\n";
    output << "    \"deleted\": " << summary.deleted << ",\n";
    output << "    \"unchanged\": " << summary.unchanged << ",\n";
    output << "    \"failed\": " << summary.failed << ",\n";
    output << "    \"folders_tracked\": " << summary.folders_tracked << ",\n";
    output << "    \"folders_changed\": " << summary.folders_changed << ",\n";
    output << "    \"similarity_prioritization\": " << (summary.similarity_prioritization_enabled ? "true" : "false") << ",\n";
    output << "    \"similarity_groups\": " << summary.similarity_groups << ",\n";
    output << "    \"largest_similarity_group\": " << summary.largest_similarity_group << "\n";
    output << "  },\n";
    output << "  \"symbols\": {\n";
    output << "    \"before\": " << summary.symbols_before << ",\n";
    output << "    \"after\": " << summary.symbols_after << ",\n";
    output << "    \"added\": " << summary.symbols_added << ",\n";
    output << "    \"updated\": " << summary.symbols_updated << ",\n";
    output << "    \"deleted\": " << summary.symbols_deleted << ",\n";
    output << "    \"deactivated\": " << summary.symbols_deactivated << ",\n";
    output << "    \"parsed_files\": " << summary.parse_counts.parsed << ",\n";
    output << "    \"skipped_files\": " << summary.parse_counts.skipped << ",\n";
    output << "    \"failed_files\": " << summary.parse_counts.failed << "\n";
    output << "  },\n";
    output << "  \"database\": {\n";
    output << "    \"path\": \"" << json_escape(summary.database_path) << "\",\n";
    output << "    \"size_before_bytes\": " << summary.database_size_before << ",\n";
    output << "    \"size_after_bytes\": " << summary.database_size_after << ",\n";
    output << "    \"growth_bytes\": " << (summary.database_size_after - summary.database_size_before) << "\n";
    output << "  },\n";
    output << "  \"snapshot\": {\n";
    output << "    \"id\": " << summary.snapshot_id << ",\n";
    output << "    \"created_at\": \"" << json_escape(summary.finished_at) << "\",\n";
    output << "    \"repository_fingerprint_before\": \"" << json_escape(summary.repo_fingerprint_before) << "\",\n";
    output << "    \"repository_fingerprint_after\": \"" << json_escape(summary.repo_fingerprint_after) << "\"\n";
    output << "  },\n";
    output << "  \"diff\": {\n";
    output << "    \"reindexed_files\": " << string_array_json(summary.reindexed_files, "      ") << ",\n";
    output << "    \"deleted_files\": " << string_array_json(summary.deleted_files, "      ") << ",\n";
    output << "    \"changed_folders\": " << string_array_json(summary.changed_folders, "      ") << ",\n";
    output << "    \"similarity_groups\": [\n";
    bool wrote_similarity_group = false;
    for (const auto& group : summary.similarity_group_summaries) {
        if (group.files.size() < 2) {
            continue;
        }
        if (wrote_similarity_group) {
            output << ",\n";
        }
        wrote_similarity_group = true;
        output << "      {\"id\": " << group.id << ", \"signature\": \"" << json_escape(group.representative_signature)
               << "\", \"files\": " << string_array_json(group.files, "        ") << "}";
    }
    output << "\n    ]\n";
    output << "  },\n";
    output << "  \"warnings\": " << string_array_json(summary.warnings, "    ") << "\n";
    output << "}\n";
    return output.str();
}

std::string status_json(const std::filesystem::path& index_dir)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();

    std::ostringstream output;
    output << "{\n";
    if (status) {
        output
            << "  \"repo_root\": \"" << json_escape(status->repo_root) << "\",\n"
            << "  \"index_root\": \"" << json_escape(status->index_root) << "\",\n"
            << "  \"database\": \"" << json_escape(database_path.string()) << "\",\n"
            << "  \"schema_version\": " << status->schema_version << ",\n"
            << "  \"last_indexed_at\": \"" << json_escape(status->last_indexed_at) << "\"\n";
    } else {
        output
            << "  \"repo_root\": \"\",\n"
            << "  \"index_root\": \"" << json_escape(canonical_existing_directory(index_dir, "index_dir").string()) << "\",\n"
            << "  \"database\": \"" << json_escape(database_path.string()) << "\",\n"
            << "  \"schema_version\": 0,\n"
            << "  \"last_indexed_at\": \"\"\n";
    }
    output << "}\n";
    return output.str();
}


repolens::ContextDescription stored_or_generate_file_description(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const std::string& file_path,
    bool refresh,
    const ContextDescriptionProvider& provider)
{
    const auto existing = database.context_description_for(repository_id, "file", 0, file_path);
    if (existing && !refresh) {
        return *existing;
    }

    repolens::ContextDescription description;
    description.target_type = "file";
    description.target_id = 0;
    description.target_key = file_path;
    description.source = "deterministic";
    description.description = provider.describe_file(file_path, database.symbols_for_file(repository_id, file_path));
    database.upsert_context_description(repository_id, description);
    return database.context_description_for(repository_id, description.target_type, description.target_id, description.target_key).value_or(description);
}

repolens::ContextDescription stored_or_generate_symbol_description(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const repolens::FactSymbol& symbol,
    bool refresh,
    const ContextDescriptionProvider& provider)
{
    const auto existing = database.context_description_for(repository_id, "symbol", symbol.row_id);
    if (existing && !refresh) {
        return *existing;
    }

    repolens::ContextDescription description;
    description.target_type = "symbol";
    description.target_id = symbol.row_id;
    description.target_key = symbol.stable_id;
    description.source = "deterministic";
    description.description = provider.describe_symbol(symbol);
    database.upsert_context_description(repository_id, description);
    return database.context_description_for(repository_id, description.target_type, description.target_id, description.target_key).value_or(description);
}

repolens::ContextDescription stored_or_generate_symbol_description(
    repolens::SqliteDatabase& database,
    long long repository_id,
    const repolens::ContextSymbolCandidate& symbol,
    bool refresh,
    const ContextDescriptionProvider& provider)
{
    const auto existing = database.context_description_for(repository_id, "symbol", symbol.symbol_id);
    if (existing && !refresh) {
        return *existing;
    }

    repolens::ContextDescription description;
    description.target_type = "symbol";
    description.target_id = symbol.symbol_id;
    description.target_key = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
    description.source = "deterministic";
    description.description = provider.describe_symbol(symbol);
    database.upsert_context_description(repository_id, description);
    return database.context_description_for(repository_id, description.target_type, description.target_id, description.target_key).value_or(description);
}

void print_descriptions_text(const std::vector<repolens::ContextDescription>& descriptions)
{
    for (const auto& description : descriptions) {
        std::cout
            << description.target_type << ": "
            << (description.target_key.empty() ? std::to_string(description.target_id) : description.target_key) << '\n'
            << "source: " << description.source << '\n'
            << description.description << "\n\n";
    }
}

int run_describe(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens describe --index-dir <index_path> [--file <path>|--symbol <name>|--all] [--deterministic] [--refresh] [--format text|json|--json]");
    }

    const bool json = has_flag(argc, argv, "--json") || read_option_string(argc, argv, "--format").value_or("text") == "json";
    const bool refresh = has_flag(argc, argv, "--refresh") || has_flag(argc, argv, "--deterministic");
    const auto file_path = read_option_string(argc, argv, "--file");
    auto symbol_name = read_option_string(argc, argv, "--symbol");
    if ((!symbol_name || symbol_name->empty()) && !file_path && !has_flag(argc, argv, "--all")) {
        symbol_name = positional_after_command(argc, argv);
    }

    const auto database_path = database_path_from_index(*index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    DeterministicContextDescriptionProvider provider;
    std::vector<repolens::ContextDescription> descriptions;

    if (file_path && !file_path->empty()) {
        descriptions.push_back(stored_or_generate_file_description(database, status->repository_id, *file_path, refresh, provider));
    } else if (symbol_name && !symbol_name->empty()) {
        const auto symbols = database.resolve_symbols(status->repository_id, *symbol_name, std::nullopt);
        if (symbols.empty()) {
            throw std::runtime_error("Symbol not found: " + *symbol_name);
        }
        for (const auto& symbol : symbols) {
            descriptions.push_back(stored_or_generate_symbol_description(database, status->repository_id, symbol, refresh, provider));
        }
    } else if (has_flag(argc, argv, "--all")) {
        for (const auto& path : database.active_file_paths(status->repository_id)) {
            descriptions.push_back(stored_or_generate_file_description(database, status->repository_id, path, refresh, provider));
        }
        for (const auto& symbol : database.active_context_symbols(status->repository_id)) {
            descriptions.push_back(stored_or_generate_symbol_description(database, status->repository_id, symbol, refresh, provider));
        }
    } else {
        throw std::runtime_error("Usage: repolens describe --index-dir <index_path> [--file <path>|--symbol <name>|--all] [--deterministic] [--refresh] [--format text|json|--json]");
    }

    std::sort(descriptions.begin(), descriptions.end(), [](const auto& left, const auto& right) {
        return std::tie(left.target_type, left.target_key, left.target_id) < std::tie(right.target_type, right.target_key, right.target_id);
    });

    if (json) {
        std::cout << description_json(descriptions);
    } else {
        print_descriptions_text(descriptions);
    }
    return 0;
}

int run_diagnostics(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens diagnostics --index-dir <index_path>");
    }

    const auto database_path = database_path_from_index(*index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto counts = database.count_rows();

    std::cout
        << "RepoLens Diagnostics\n"
        << "--------------------\n"
        << "Database:           " << database_path.string() << '\n'
        << "Database size:      " << format_bytes(file_size_or_zero(database_path)) << "\n\n"
        << "Rows:\n"
        << "repositories:       " << counts.repositories << '\n'
        << "files:              " << counts.files << '\n'
        << "symbols:            " << counts.symbols << '\n'
        << "symbol_parameters:  " << counts.symbol_parameters << '\n'
        << "symbol_relations:   " << counts.symbol_relations << '\n'
        << "symbol_references:  " << counts.symbol_references << '\n'
        << "context_descriptions:" << counts.context_descriptions << '\n'
        << "virtual_files:" << counts.virtual_files << '\n'
        << "scip_imports:       " << counts.scip_imports << '\n'
        << "snapshots:          " << counts.snapshots << '\n'
        << "changes:            " << counts.changes << '\n';

    return 0;
}

int run_compact(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens compact --index-dir <index_path>");
    }

    const auto database_path = database_path_from_index(*index_dir);
    const auto size_before = file_size_or_zero(database_path);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    database.compact();
    const auto size_after = file_size_or_zero(database_path);

    std::cout
        << "Compaction complete\n"
        << "Database:     " << database_path.string() << '\n'
        << "Size before:  " << format_bytes(size_before) << '\n'
        << "Size after:   " << format_bytes(size_after) << '\n'
        << "Reclaimed:    " << format_bytes(std::max(0LL, size_before - size_after)) << '\n';
    return 0;
}

std::optional<std::string> json_string_value(const std::string& body, const std::string& key)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position = body.find(':', position + key_text.size());
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position = body.find('"', position + 1);
    if (position == std::string::npos) {
        return std::nullopt;
    }

    std::string value;
    bool escaped = false;
    for (std::size_t index = position + 1; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            switch (character) {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(character);
                break;
            }
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            return value;
        }
        value.push_back(character);
    }

    return std::nullopt;
}

int json_int_value(const std::string& body, const std::string& key, int fallback)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return fallback;
    }
    position = body.find(':', position + key_text.size());
    if (position == std::string::npos) {
        return fallback;
    }
    ++position;
    while (position < body.size() && std::isspace(static_cast<unsigned char>(body[position]))) {
        ++position;
    }

    std::size_t end = position;
    while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) || body[end] == '-')) {
        ++end;
    }
    if (end == position) {
        return fallback;
    }

    return std::stoi(body.substr(position, end - position));
}


double json_double_value(const std::string& body, const std::string& key, double fallback)
{
    if (const auto text = json_string_value(body, key)) {
        return std::stod(*text);
    }
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return fallback;
    }
    position = body.find(':', position + key_text.size());
    if (position == std::string::npos) {
        return fallback;
    }
    ++position;
    while (position < body.size() && std::isspace(static_cast<unsigned char>(body[position])) != 0) {
        ++position;
    }
    std::size_t end = position;
    while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) || body[end] == '-' || body[end] == '+' || body[end] == '.')) {
        ++end;
    }
    if (end == position) {
        return fallback;
    }
    return std::stod(body.substr(position, end - position));
}

bool json_bool_value(const std::string& body, const std::string& key, bool fallback)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return fallback;
    }
    position = body.find(':', position + key_text.size());
    if (position == std::string::npos) {
        return fallback;
    }
    ++position;
    while (position < body.size() && std::isspace(static_cast<unsigned char>(body[position]))) {
        ++position;
    }

    if (body.compare(position, 4, "true") == 0) {
        return true;
    }
    if (body.compare(position, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

std::vector<std::string> json_string_array_value(const std::string& body, const std::string& key)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return {};
    }
    position = body.find('[', position + key_text.size());
    if (position == std::string::npos) {
        const auto single_value = json_string_value(body, key);
        return single_value ? split_symbols(*single_value) : std::vector<std::string>{};
    }

    const auto end = body.find(']', position + 1);
    if (end == std::string::npos) {
        return {};
    }

    std::vector<std::string> values;
    std::size_t cursor = position + 1;
    while (cursor < end) {
        const auto quote = body.find('"', cursor);
        if (quote == std::string::npos || quote >= end) {
            break;
        }
        const auto close = body.find('"', quote + 1);
        if (close == std::string::npos || close > end) {
            break;
        }
        values.push_back(body.substr(quote + 1, close - quote - 1));
        cursor = close + 1;
    }

    return values;
}


std::optional<std::string> json_object_value(const std::string& body, const std::string& key)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position = body.find(":", position + key_text.size());
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position = body.find("{", position + 1);
    if (position == std::string::npos) {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = position; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
            if (depth == 0) {
                return body.substr(position, index - position + 1);
            }
        }
    }
    return std::nullopt;
}

std::string json_raw_value(const std::string& body, const std::string& key)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return "null";
    }
    position = body.find(":", position + key_text.size());
    if (position == std::string::npos) {
        return "null";
    }
    ++position;
    while (position < body.size() && std::isspace(static_cast<unsigned char>(body[position])) != 0) {
        ++position;
    }
    if (position >= body.size()) {
        return "null";
    }
    if (body[position] == '"') {
        bool escaped = false;
        for (std::size_t index = position + 1; index < body.size(); ++index) {
            const char character = body[index];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (character == '\\') {
                escaped = true;
                continue;
            }
            if (character == '"') {
                return body.substr(position, index - position + 1);
            }
        }
        return "null";
    }
    std::size_t end = position;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && std::isspace(static_cast<unsigned char>(body[end])) == 0) {
        ++end;
    }
    const auto value = body.substr(position, end - position);
    return value.empty() ? std::string{"null"} : value;
}

bool json_top_level_has_key(const std::string& body, const std::string& key)
{
    const std::string key_text = "\"" + key + "\"";
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t string_start = std::string::npos;

    for (std::size_t index = 0; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            if (!in_string) {
                string_start = index;
                in_string = true;
            } else {
                in_string = false;
                if (depth == 1 && body.compare(string_start, key_text.size(), key_text) == 0) {
                    std::size_t cursor = index + 1;
                    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                        ++cursor;
                    }
                    if (cursor < body.size() && body[cursor] == ':') {
                        return true;
                    }
                }
            }
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
        }
    }
    return false;
}

std::string trim_copy(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> json_top_level_raw_value(const std::string& body, const std::string& key)
{
    const std::string key_text = "\"" + key + "\"";
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t string_start = std::string::npos;

    for (std::size_t index = 0; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            if (!in_string) {
                string_start = index;
                in_string = true;
            } else {
                in_string = false;
                if (depth == 1 && body.compare(string_start, key_text.size(), key_text) == 0) {
                    std::size_t cursor = index + 1;
                    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                        ++cursor;
                    }
                    if (cursor >= body.size() || body[cursor] != ':') {
                        return std::nullopt;
                    }
                    ++cursor;
                    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                        ++cursor;
                    }
                    const std::size_t value_start = cursor;
                    int nested_depth = 0;
                    bool value_in_string = false;
                    bool value_escaped = false;
                    for (; cursor < body.size(); ++cursor) {
                        const char value_character = body[cursor];
                        if (value_escaped) {
                            value_escaped = false;
                            continue;
                        }
                        if (value_character == '\\') {
                            value_escaped = value_in_string;
                            continue;
                        }
                        if (value_character == '"') {
                            value_in_string = !value_in_string;
                            continue;
                        }
                        if (value_in_string) {
                            continue;
                        }
                        if (value_character == '{' || value_character == '[') {
                            ++nested_depth;
                        } else if (value_character == '}' || value_character == ']') {
                            if (nested_depth == 0) {
                                break;
                            }
                            --nested_depth;
                        } else if (value_character == ',' && nested_depth == 0) {
                            break;
                        }
                    }
                    return trim_copy(body.substr(value_start, cursor - value_start));
                }
            }
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
        }
    }
    return std::nullopt;
}

bool json_raw_is_string(const std::string& raw)
{
    const auto value = trim_copy(raw);
    return value.size() >= 2 && value.front() == '"' && value.back() == '"';
}

bool json_raw_is_object(const std::string& raw)
{
    const auto value = trim_copy(raw);
    return value.size() >= 2 && value.front() == '{' && value.back() == '}';
}

bool json_raw_is_valid_id(const std::string& raw)
{
    const auto value = trim_copy(raw);
    if (value == "null") {
        return false;
    }
    if (json_raw_is_string(value)) {
        return true;
    }
    if (value.empty()) {
        return false;
    }
    std::size_t index = value[0] == '-' ? 1 : 0;
    if (index >= value.size()) {
        return false;
    }
    for (; index < value.size(); ++index) {
        if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

bool json_text_looks_like_object(const std::string& body)
{
    const auto value = trim_copy(body);
    return value.size() >= 2 && value.front() == '{' && value.back() == '}';
}

std::vector<std::string> json_top_level_keys(const std::string& body)
{
    std::vector<std::string> keys;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t string_start = std::string::npos;

    for (std::size_t index = 0; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            if (!in_string) {
                string_start = index;
                in_string = true;
            } else {
                in_string = false;
                if (depth == 1) {
                    std::size_t cursor = index + 1;
                    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
                        ++cursor;
                    }
                    if (cursor < body.size() && body[cursor] == ':') {
                        keys.push_back(body.substr(string_start + 1, index - string_start - 1));
                    }
                }
            }
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{') {
            ++depth;
        } else if (character == '}') {
            --depth;
        }
    }
    return keys;
}

bool json_object_has_only_keys(const std::string& body, const std::vector<std::string>& allowed_keys)
{
    const auto keys = json_top_level_keys(body);
    return std::all_of(keys.begin(), keys.end(), [&](const std::string& key) {
        return std::find(allowed_keys.begin(), allowed_keys.end(), key) != allowed_keys.end();
    });
}
std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> json_object_array_value(const std::string& body, const std::string& key)
{
    const auto key_text = "\"" + key + "\"";
    auto position = body.find(key_text);
    if (position == std::string::npos) {
        return {};
    }
    position = body.find('[', position + key_text.size());
    if (position == std::string::npos) {
        return {};
    }

    std::vector<std::string> objects;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t object_start = std::string::npos;
    for (std::size_t index = position + 1; index < body.size(); ++index) {
        const char character = body[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = in_string;
            continue;
        }
        if (character == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{') {
            if (depth == 0) {
                object_start = index;
            }
            ++depth;
        } else if (character == '}') {
            --depth;
            if (depth == 0 && object_start != std::string::npos) {
                objects.push_back(body.substr(object_start, index - object_start + 1));
                object_start = std::string::npos;
            }
        } else if (character == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

std::string scip_import_summary_json(const repolens::ScipImportSummary& summary)
{
    std::ostringstream output;
    output
        << "{\n"
        << "  \"source_path\": \"" << json_escape(summary.source_path) << "\",\n"
        << "  \"symbols_seen\": " << summary.symbols_seen << ",\n"
        << "  \"symbols_mapped\": " << summary.symbols_mapped << ",\n"
        << "  \"symbols_inserted\": " << summary.symbols_inserted << ",\n"
        << "  \"references_seen\": " << summary.references_seen << ",\n"
        << "  \"references_inserted\": " << summary.references_inserted << ",\n"
        << "  \"relationships_inserted\": " << summary.relationships_inserted << ",\n"
        << "  \"unresolved_references\": " << summary.unresolved_references << ",\n"
        << "  \"conflicts\": " << summary.conflicts << "\n"
        << "}\n";
    return output.str();
}

void print_scip_import_summary(const repolens::ScipImportSummary& summary)
{
    std::cout
        << "SCIP import complete\n"
        << "Source: " << summary.source_path << '\n'
        << "Symbols seen: " << summary.symbols_seen << '\n'
        << "Symbols mapped: " << summary.symbols_mapped << '\n'
        << "Symbols inserted: " << summary.symbols_inserted << '\n'
        << "References seen: " << summary.references_seen << '\n'
        << "References inserted: " << summary.references_inserted << '\n'
        << "Relationships inserted: " << summary.relationships_inserted << '\n'
        << "Unresolved references: " << summary.unresolved_references << '\n'
        << "Conflicts: " << summary.conflicts << '\n';
}

repolens::ScipImportSummary import_scip_index_file(const std::filesystem::path& index_dir, const std::filesystem::path& scip_path)
{
    const auto index_root = canonical_existing_directory(index_dir, "index_dir");
    const auto input_path = canonical_existing_file(scip_path, "scip_index");
    const auto text = read_text_file(input_path);
    const auto trimmed = trim_config_line(text);
    if (trimmed.empty() || (trimmed.front() != '{' && trimmed.front() != '[')) {
        throw std::runtime_error("Binary .scip parsing is not built in. Export SCIP data to RepoLens JSON format first.");
    }

    std::vector<repolens::ScipSymbolFact> symbols;
    for (const auto& object : json_object_array_value(text, "symbols")) {
        repolens::ScipSymbolFact symbol;
        symbol.scip_symbol = json_string_value(object, "scip_symbol").value_or(json_string_value(object, "symbol").value_or(""));
        symbol.kind = json_string_value(object, "kind").value_or("symbol");
        symbol.name = json_string_value(object, "name").value_or("");
        symbol.qualified_name = json_string_value(object, "qualified_name").value_or(json_string_value(object, "display_name").value_or(""));
        symbol.signature = json_string_value(object, "signature").value_or("");
        symbol.file_path = json_string_value(object, "file").value_or(json_string_value(object, "file_path").value_or(""));
        symbol.language = json_string_value(object, "language").value_or("scip");
        symbol.line_start = json_int_value(object, "line_start", json_int_value(object, "start_line", 0));
        symbol.line_end = json_int_value(object, "line_end", json_int_value(object, "end_line", symbol.line_start));
        symbols.push_back(std::move(symbol));
    }

    std::vector<repolens::ScipReferenceFact> references;
    for (const auto& object : json_object_array_value(text, "references")) {
        repolens::ScipReferenceFact reference;
        reference.source_symbol = json_string_value(object, "source_symbol").value_or(json_string_value(object, "source").value_or(""));
        reference.target_symbol = json_string_value(object, "target_symbol").value_or(json_string_value(object, "target").value_or(""));
        reference.source_file = json_string_value(object, "file").value_or(json_string_value(object, "source_file").value_or(""));
        reference.language = json_string_value(object, "language").value_or("scip");
        reference.line = json_int_value(object, "line", 0);
        reference.column = json_int_value(object, "column", json_int_value(object, "column_number", 0));
        reference.reference_text = json_string_value(object, "reference_text").value_or(json_string_value(object, "text").value_or(reference.target_symbol));
        reference.relationship_type = json_string_value(object, "relationship_type").value_or(json_string_value(object, "type").value_or("references"));
        reference.unresolved = json_bool_value(object, "unresolved", false);
        references.push_back(std::move(reference));
    }

    repolens::SqliteDatabase database{database_path_from_index(index_root)};
    database.create_schema();
    const auto status = read_required_status(database);
    return database.import_scip_facts(status.repository_id, input_path.string(), symbols, references);
}

std::string signal_type_normalized(std::string value)
{
    value = ascii_lower(std::move(value));
    std::string normalized;
    for (char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_') {
            normalized.push_back(character);
        }
    }
    if (normalized.empty()) {
        throw std::runtime_error("Signal type must contain letters, digits, '-' or '_'.");
    }
    return normalized;
}

std::string signal_virtual_path(const std::string& signal_type, const std::filesystem::path& source_file)
{
    const auto absolute = std::filesystem::absolute(source_file).lexically_normal().string();
    auto filename = source_file.filename().string();
    if (filename.empty()) {
        filename = "signal.txt";
    }
    for (char& character : filename) {
        if (character == '\\' || character == '/' || character == ':') {
            character = '_';
        }
    }
    return "signals/" + signal_type + "/" + fnv1a_hex(absolute) + "/" + filename;
}

std::string read_signal_text(const std::filesystem::path& path, long long max_bytes, bool& truncated)
{
    if (max_bytes <= 0) {
        max_bytes = 1024 * 1024;
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read signal file: " + path.string());
    }
    std::string content;
    content.resize(static_cast<std::size_t>(max_bytes));
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    truncated = !file.eof();
    return content;
}

long long signal_line_count(const std::string& content)
{
    if (content.empty()) {
        return 0;
    }
    return static_cast<long long>(std::count(content.begin(), content.end(), '\n')) + (content.back() == '\n' ? 0 : 1);
}

std::optional<std::string> positional_after_signals_subcommand(int argc, char* argv[])
{
    for (int index = 3; index < argc; ++index) {
        const std::string_view value{argv[index]};
        if (!value.empty() && value[0] == '-') {
            ++index;
            continue;
        }
        return std::string{argv[index]};
    }
    return std::nullopt;
}

repolens::VirtualFileFact import_signal_file(
    const std::filesystem::path& index_dir,
    const std::string& signal_type,
    const std::filesystem::path& source_file,
    long long max_bytes)
{
    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    bool truncated = false;
    const auto content = read_signal_text(source_file, max_bytes, truncated);
    const auto type = signal_type_normalized(signal_type);
    repolens::VirtualFileInput input;
    input.signal_type = type;
    input.source_path = std::filesystem::absolute(source_file).lexically_normal().string();
    input.virtual_path = signal_virtual_path(type, source_file);
    input.content_hash = fnv1a_hex(content);
    input.content = content;
    input.size_bytes = static_cast<long long>(content.size());
    input.line_count = signal_line_count(content);
    input.truncated = truncated;
    return database.upsert_virtual_file(status->repository_id, input);
}

std::vector<repolens::VirtualFileFact> list_signal_files(const std::filesystem::path& index_dir)
{
    repolens::SqliteDatabase database{database_path_from_index(index_dir)};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }
    return database.list_virtual_files(status->repository_id);
}

std::vector<repolens::VirtualFileSearchResult> search_signal_files(const std::filesystem::path& index_dir, const std::string& query, int limit)
{
    repolens::SqliteDatabase database{database_path_from_index(index_dir)};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }
    return database.search_virtual_files(status->repository_id, query, limit);
}

bool delete_signal_file(const std::filesystem::path& index_dir, const std::string& path)
{
    repolens::SqliteDatabase database{database_path_from_index(index_dir)};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }
    if (database.delete_virtual_file(status->repository_id, path)) {
        return true;
    }
    const auto normalized_path = std::filesystem::absolute(std::filesystem::path{path}).lexically_normal().string();
    if (normalized_path != path) {
        return database.delete_virtual_file(status->repository_id, normalized_path);
    }
    return false;
}

std::string virtual_file_json_object(const repolens::VirtualFileFact& file, const std::string& indent)
{
    std::ostringstream output;
    output << indent << "{\n"
           << indent << "  \"id\": " << file.id << ",\n"
           << indent << "  \"type\": \"" << json_escape(file.signal_type) << "\",\n"
           << indent << "  \"virtual_path\": \"" << json_escape(file.virtual_path) << "\",\n"
           << indent << "  \"source_path\": \"" << json_escape(file.source_path) << "\",\n"
           << indent << "  \"imported_at\": \"" << json_escape(file.imported_at) << "\",\n"
           << indent << "  \"content_hash\": \"" << json_escape(file.content_hash) << "\",\n"
           << indent << "  \"size_bytes\": " << file.size_bytes << ",\n"
           << indent << "  \"line_count\": " << file.line_count << ",\n"
           << indent << "  \"truncated\": " << (file.truncated ? "true" : "false") << "\n"
           << indent << "}";
    return output.str();
}

std::string virtual_file_list_json(const std::vector<repolens::VirtualFileFact>& files)
{
    std::ostringstream output;
    output << "{\n  \"signals\": [\n";
    for (std::size_t index = 0; index < files.size(); ++index) {
        output << virtual_file_json_object(files[index], "    ");
        if (index + 1 < files.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::string virtual_file_search_json(const std::vector<repolens::VirtualFileSearchResult>& results)
{
    std::ostringstream output;
    output << "{\n  \"results\": " << virtual_file_search_json_array(results, "    ") << "\n}\n";
    return output.str();
}

void print_virtual_file_list_text(const std::vector<repolens::VirtualFileFact>& files)
{
    if (files.empty()) {
        std::cout << "No signals imported.\n";
        return;
    }
    for (const auto& file : files) {
        std::cout << file.virtual_path << " type=" << file.signal_type
                  << " lines=" << file.line_count << " bytes=" << file.size_bytes
                  << " hash=" << file.content_hash << " source=" << file.source_path;
        if (file.truncated) {
            std::cout << " truncated=true";
        }
        std::cout << '\n';
    }
}

void print_virtual_file_search_text(const std::vector<repolens::VirtualFileSearchResult>& results)
{
    if (results.empty()) {
        std::cout << "No signal matches.\n";
        return;
    }
    for (const auto& result : results) {
        std::cout << result.file.virtual_path << ':' << result.line << " [" << result.file.signal_type << "] " << result.snippet << '\n';
    }
}

int run_signals(int argc, char* argv[])
{
    if (argc < 3) {
        throw std::runtime_error("Usage: repolens signals <import|list|search|delete> --index-dir <index_path> [...]");
    }
    const std::string subcommand{argv[2]};
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("signals requires --index-dir <index_path>.");
    }
    const auto format = fact_output_format(argc, argv);

    if (subcommand == "import") {
        const auto type = read_option_string(argc, argv, "--type");
        const auto file = read_option_path(argc, argv, "--file");
        if (!type || type->empty() || !file) {
            throw std::runtime_error("Usage: repolens signals import --index-dir <index_path> --type <terminal|build-log|test-output|chat|notes|summary> --file <path> [--max-bytes <n>] [--json]");
        }
        const auto imported = import_signal_file(*index_dir, *type, *file, read_option_int(argc, argv, "--max-bytes", 1024 * 1024));
        if (format == "json") {
            std::cout << "{\n  \"imported\": " << virtual_file_json_object(imported, "  ") << "\n}\n";
        } else {
            std::cout << "Imported signal: " << imported.virtual_path << "\n";
        }
        return 0;
    }

    if (subcommand == "list") {
        const auto files = list_signal_files(*index_dir);
        if (format == "json") {
            std::cout << virtual_file_list_json(files);
        } else {
            print_virtual_file_list_text(files);
        }
        return 0;
    }

    if (subcommand == "search") {
        auto query = read_option_string(argc, argv, "--query");
        if (!query || query->empty()) {
            query = positional_after_signals_subcommand(argc, argv);
        }
        if (!query || query->empty()) {
            throw std::runtime_error("Usage: repolens signals search --index-dir <index_path> <query> [--limit <n>] [--json]");
        }
        const auto results = search_signal_files(*index_dir, *query, read_option_int(argc, argv, "--limit", 20));
        if (format == "json") {
            std::cout << virtual_file_search_json(results);
        } else {
            print_virtual_file_search_text(results);
        }
        return 0;
    }

    if (subcommand == "delete") {
        auto target = read_option_string(argc, argv, "--path");
        if (!target || target->empty()) {
            target = read_option_string(argc, argv, "--file");
        }
        if (!target || target->empty()) {
            throw std::runtime_error("Usage: repolens signals delete --index-dir <index_path> --path <virtual-or-source-path> [--json]");
        }
        const bool deleted = delete_signal_file(*index_dir, *target);
        if (format == "json") {
            std::cout << "{\n  \"deleted\": " << (deleted ? "true" : "false") << "\n}\n";
        } else {
            std::cout << (deleted ? "Deleted signal.\n" : "Signal not found.\n");
        }
        return deleted ? 0 : 1;
    }

    throw std::runtime_error("Unknown signals subcommand: " + subcommand);
}
int run_import_scip(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    std::filesystem::path scip_path;
    if (const auto option_path = read_option_path(argc, argv, "--scip-index")) {
        scip_path = *option_path;
    } else if (const auto positional = first_positional_after_command(argc, argv)) {
        scip_path = *positional;
    }
    if (!index_dir || scip_path.empty()) {
        throw std::runtime_error("Usage: repolens import-scip --index-dir <index_path> <scip-json-path> [--json]");
    }
    const auto summary = import_scip_index_file(*index_dir, scip_path);
    if (has_flag(argc, argv, "--json") || read_option_string(argc, argv, "--format").value_or("text") == "json") {
        std::cout << scip_import_summary_json(summary);
    } else {
        print_scip_import_summary(summary);
    }
    return 0;
}

std::string mcp_json_rpc_result(const std::string& id, const std::string& result_json)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result_json + "}\n";
}

std::string mcp_json_rpc_error(const std::string& id, int code, const std::string& message)
{
    std::ostringstream output;
    output << "{\"jsonrpc\":\"2.0\",\"id\":" << id
           << ",\"error\":{\"code\":" << code
           << ",\"message\":\"" << json_escape(message) << "\"}}\n";
    return output.str();
}

std::size_t json_top_level_array_count(const std::string& body, const std::string& key)
{
    const auto raw = json_top_level_raw_value(body, key);
    if (!raw) {
        return 0;
    }
    const auto value = trim_copy(*raw);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
        return 0;
    }

    std::size_t count = 0;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    bool has_value = false;
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        const char character = value[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = in_string;
            continue;
        }
        if (character == '"') {
            in_string = !in_string;
            has_value = true;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (character == '{' || character == '[') {
            ++depth;
            has_value = true;
        } else if (character == '}' || character == ']') {
            --depth;
        } else if (character == ',' && depth == 0) {
            if (has_value) {
                ++count;
                has_value = false;
            }
        } else if (std::isspace(static_cast<unsigned char>(character)) == 0) {
            has_value = true;
        }
    }
    return has_value ? count + 1 : count;
}

std::string pluralize(std::size_t count, const std::string& singular, const std::string& plural)
{
    return std::to_string(count) + ' ' + (count == 1 ? singular : plural);
}

std::string json_minify(const std::string& json_text)
{
    std::string output;
    output.reserve(json_text.size());
    bool in_string = false;
    bool escaped = false;
    for (const char character : json_text) {
        if (escaped) {
            output.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\') {
            output.push_back(character);
            escaped = in_string;
            continue;
        }
        if (character == '"') {
            output.push_back(character);
            in_string = !in_string;
            continue;
        }
        if (!in_string && std::isspace(static_cast<unsigned char>(character)) != 0) {
            continue;
        }
        output.push_back(character);
    }
    return output;
}

std::string mcp_success_summary(const std::string& tool_name, const std::string& structured_json)
{
    if (tool_name == "resolve_symbol") {
        return "Resolved " + pluralize(json_top_level_array_count(structured_json, "symbols"), "matching symbol", "matching symbols") + ". Results are available in structuredContent.symbols.";
    }
    if (tool_name == "get_snippet") {
        return "Retrieved source snippet. Exact text is available in structuredContent.code.";
    }
    if (tool_name == "compact_view") {
        return "Generated compact structural view. Tree data is available in structuredContent.views.";
    }
    if (tool_name == "search") {
        return "Found " + pluralize(json_top_level_array_count(structured_json, "results"), "search result", "search results") + ". Results are available in structuredContent.results.";
    }
    if (tool_name == "refs") {
        return "Found " + pluralize(json_top_level_array_count(structured_json, "references"), "reference", "references") + ". References are available in structuredContent.references.";
    }
    if (tool_name == "trace") {
        return "Completed graph trace. Nodes, edges, paths, and truncation metadata are available in structuredContent.";
    }
    if (tool_name == "impact") {
        return "Completed impact analysis. Affected symbols, files, paths, and truncation metadata are available in structuredContent.";
    }
    if (tool_name == "architecture_hubs") {
        return "Ranked architectural hubs. Results are available in structuredContent.hubs.";
    }
    if (tool_name == "architecture_communities") {
        return "Computed architectural communities. Results are available in structuredContent.communities.";
    }
    if (tool_name == "schema_info") {
        return "Loaded schema information. Tables and row counts are available in structuredContent.";
    }
    if (tool_name == "status") {
        return "Loaded RepoLens index status. Repository metadata is available in structuredContent.";
    }
    if (tool_name == "quality") {
        return "Generated deterministic quality report. Findings and summary are available in structuredContent.";
    }
    return "Tool completed. Structured data is available in structuredContent.";
}

std::string append_json_object_fields(const std::string& json_object, const std::string& fields)
{
    auto value = trim_copy(json_object);
    if (value.size() < 2 || value.front() != '{' || value.back() != '}') {
        return json_minify(json_object);
    }
    if (fields.empty()) {
        return json_minify(value);
    }
    value.pop_back();
    const bool has_existing_fields = value.find_first_not_of(" {\t\r\n") != std::string::npos;
    if (has_existing_fields) {
        value += ',';
    }
    value += fields;
    value += '}';
    return json_minify(value);
}
std::string mcp_structured_content_json(const std::string& tool_name, const std::string& structured_json)
{
    if (structured_json.find("\"truncated\"") != std::string::npos) {
        return json_minify(structured_json);
    }

    if (tool_name == "search") {
        const auto count = json_top_level_array_count(structured_json, "results");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedCount\":" + std::to_string(count) + ",\"totalCount\":" + std::to_string(count));
    }
    if (tool_name == "refs") {
        const auto count = json_top_level_array_count(structured_json, "references");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedCount\":" + std::to_string(count) + ",\"totalCount\":" + std::to_string(count));
    }
    if (tool_name == "trace") {
        const auto node_count = json_top_level_array_count(structured_json, "nodes");
        const auto edge_count = json_top_level_array_count(structured_json, "edges");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedNodes\":" + std::to_string(node_count) + ",\"returnedEdges\":" + std::to_string(edge_count));
    }
    if (tool_name == "impact") {
        const auto file_count = json_top_level_array_count(structured_json, "affected_files");
        const auto symbol_count = json_top_level_array_count(structured_json, "affected_symbols");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedFiles\":" + std::to_string(file_count) + ",\"returnedSymbols\":" + std::to_string(symbol_count));
    }
    if (tool_name == "architecture_hubs") {
        const auto count = json_top_level_array_count(structured_json, "hubs");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedCount\":" + std::to_string(count));
    }
    if (tool_name == "architecture_communities") {
        const auto count = json_top_level_array_count(structured_json, "communities");
        return append_json_object_fields(structured_json, "\"truncated\":false,\"returnedCount\":" + std::to_string(count));
    }
    if (tool_name == "quality") {
        return append_json_object_fields(structured_json, "\"truncated\":false");
    }
    if (tool_name == "resolve_symbol" || tool_name == "get_snippet" || tool_name == "compact_view") {
        return append_json_object_fields(structured_json, "\"truncated\":false");
    }
    return json_minify(structured_json);
}

std::string mcp_content_result(const std::string& tool_name, const std::string& structured_json)
{
    const auto structured_content = mcp_structured_content_json(tool_name, structured_json);
    std::ostringstream output;
    output << "{\"content\":[{\"type\":\"text\",\"text\":\"" << json_escape(mcp_success_summary(tool_name, structured_content))
           << "\"}],\"structuredContent\":" << structured_content << ",\"isError\":false}";
    return output.str();
}

std::string mcp_tool_error_code(const std::string& message)
{
    if (message.find("requires") != std::string::npos) {
        return "missing_argument";
    }
    if (message.find("unexpected") != std::string::npos || message.find("must") != std::string::npos || message.find("invalid") != std::string::npos) {
        return "invalid_argument";
    }
    if (message.find("not found") != std::string::npos || message.find("does not exist") != std::string::npos) {
        return "not_found";
    }
    return "tool_error";
}

std::string mcp_tool_error_argument(const std::string& message)
{
    const std::vector<std::string> candidates{"query", "name", "symbol", "file", "start", "end", "direction", "level", "arguments"};
    for (const auto& candidate : candidates) {
        if (message.find(candidate) != std::string::npos) {
            return candidate;
        }
    }
    return {};
}

std::string mcp_tool_error_result(const std::string& message)
{
    std::ostringstream output;
    const auto argument = mcp_tool_error_argument(message);
    output << "{\"content\":[{\"type\":\"text\",\"text\":\"Tool error: " << json_escape(message)
           << "\"}],\"structuredContent\":{\"error\":{\"code\":\"" << json_escape(mcp_tool_error_code(message))
           << "\",\"message\":\"" << json_escape(message) << "\"";
    if (!argument.empty()) {
        output << ",\"argument\":\"" << json_escape(argument) << "\"";
    }
    output << "}},\"isError\":true}";
    return output.str();
}

std::string mcp_tool_schema(const std::string& properties, const std::string& required = "[]")
{
    return "{\"type\":\"object\",\"properties\":{" + properties + "},\"required\":" + required + "}";
}

std::string mcp_empty_tool_schema()
{
    return "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
}

std::string mcp_flexible_output_schema(const std::string& properties = "")
{
    if (properties.empty()) {
        return "{\"type\":\"object\",\"additionalProperties\":true}";
    }
    return "{\"type\":\"object\",\"properties\":{" + properties + "},\"additionalProperties\":true}";
}

std::string mcp_tool_annotations()
{
    return "{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}";
}

std::string mcp_tool_definition(const std::string& name, const std::string& description, const std::string& input_schema, const std::string& output_schema)
{
    return "{\"name\":\"" + json_escape(name) + "\",\"description\":\"" + json_escape(description) +
        "\",\"inputSchema\":" + input_schema + ",\"outputSchema\":" + output_schema + ",\"annotations\":" + mcp_tool_annotations() + "}";
}

const std::vector<std::string>& mcp_tool_names()
{
    static const std::vector<std::string> names{
        "resolve_symbol",
        "get_snippet",
        "compact_view",
        "search",
        "refs",
        "trace",
        "impact",
        "architecture_hubs",
        "architecture_communities",
        "schema_info",
        "status",
        "quality"
    };
    return names;
}

bool mcp_tool_exists(const std::string& tool_name)
{
    const auto& names = mcp_tool_names();
    return std::find(names.begin(), names.end(), tool_name) != names.end();
}

std::string required_string_argument(const std::string& arguments, const std::vector<std::string>& names, const std::string& message)
{
    for (const auto& name : names) {
        const auto raw = json_top_level_raw_value(arguments, name);
        if (!raw) {
            continue;
        }
        if (!json_raw_is_string(*raw)) {
            throw std::runtime_error(message);
        }
        const auto value = json_string_value(arguments, name).value_or("");
        if (value.empty()) {
            throw std::runtime_error(message);
        }
        return value;
    }
    throw std::runtime_error(message);
}

std::optional<std::string> optional_string_argument(const std::string& arguments, const std::string& name)
{
    const auto raw = json_top_level_raw_value(arguments, name);
    if (!raw) {
        return std::nullopt;
    }
    if (!json_raw_is_string(*raw)) {
        throw std::runtime_error(name + " must be a string.");
    }
    return json_string_value(arguments, name).value_or("");
}

void reject_unexpected_arguments(const std::string& tool_name, const std::string& arguments, const std::vector<std::string>& allowed_keys)
{
    if (!json_object_has_only_keys(arguments, allowed_keys)) {
        throw std::runtime_error(tool_name + " received an unexpected argument.");
    }
}

std::string mcp_tools_list_json()
{
    const auto array_schema = [](const std::string& property) {
        return "\"" + property + "\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}";
    };
    const auto alias_required_schema = [](const std::string& properties, const std::string& first, const std::string& second) {
        return "{\"type\":\"object\",\"properties\":{" + properties + "},\"anyOf\":[{\"required\":[\"" + first + "\"]},{\"required\":[\"" + second + "\"]}]}";
    };
    const std::string direction_property = "\"direction\":{\"type\":\"string\",\"enum\":[\"callees\",\"callers\",\"both\"]}";
    const std::string level_property = "\"level\":{\"type\":\"string\",\"enum\":[\"file\",\"symbol\"]}";
    const std::vector<std::string> tools{
        mcp_tool_definition("resolve_symbol", "Resolve a symbol by name and optional file path.", alias_required_schema("\"name\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"}", "name", "symbol"), mcp_flexible_output_schema(array_schema("symbols") + ",\"ambiguous\":{\"type\":\"boolean\"},\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("get_snippet", "Return an exact snippet by file path and line range.", "{\"type\":\"object\",\"properties\":{\"file\":{\"type\":\"string\"},\"start\":{\"type\":\"integer\"},\"end\":{\"type\":\"integer\"},\"line_start\":{\"type\":\"integer\"},\"line_end\":{\"type\":\"integer\"}},\"anyOf\":[{\"required\":[\"file\",\"start\",\"end\"]},{\"required\":[\"file\",\"start\",\"line_end\"]},{\"required\":[\"file\",\"line_start\",\"end\"]},{\"required\":[\"file\",\"line_start\",\"line_end\"]}]}", mcp_flexible_output_schema("\"file\":{\"type\":\"string\"},\"code\":{\"type\":\"string\"},\"line_start\":{\"type\":\"integer\"},\"line_end\":{\"type\":\"integer\"},\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("compact_view", "Return a compact structural view for a file or symbol.", alias_required_schema("\"file\":{\"type\":\"string\"},\"symbol\":{\"type\":\"string\"},\"max_depth\":{\"type\":\"integer\"},\"budget_chars\":{\"type\":\"integer\"},\"include_private\":{\"type\":\"boolean\"}", "file", "symbol"), mcp_flexible_output_schema(array_schema("views") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("search", "Search indexed files and symbols.", mcp_tool_schema("\"query\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}", "[\"query\"]"), mcp_flexible_output_schema(array_schema("results") + ",\"truncated\":{\"type\":\"boolean\"},\"returnedCount\":{\"type\":\"integer\"}")),
        mcp_tool_definition("refs", "List references for a symbol.", alias_required_schema("\"symbol\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}", "symbol", "name"), mcp_flexible_output_schema(array_schema("references") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("trace", "Trace callers, callees, or both over stored relationships.", alias_required_schema("\"symbol\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}," + direction_property + ",\"depth\":{\"type\":\"integer\"},\"min_confidence\":{\"type\":\"number\"},\"max_results\":{\"type\":\"integer\"},\"budget_chars\":{\"type\":\"integer\"}", "symbol", "name"), mcp_flexible_output_schema(array_schema("nodes") + "," + array_schema("edges") + "," + array_schema("paths") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("impact", "Analyze direct and transitive reverse dependency impact.", "{\"type\":\"object\",\"properties\":{\"symbol\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"file\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\"},\"max_results\":{\"type\":\"integer\"},\"budget_chars\":{\"type\":\"integer\"},\"include_paths\":{\"type\":\"boolean\"}},\"anyOf\":[{\"required\":[\"symbol\"]},{\"required\":[\"name\"]},{\"required\":[\"file\"]}]}", mcp_flexible_output_schema(array_schema("affected_files") + "," + array_schema("affected_symbols") + "," + array_schema("paths") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("architecture_hubs", "Rank architectural hub files or symbols.", mcp_tool_schema(level_property + ",\"top\":{\"type\":\"integer\"},\"seed\":{\"type\":\"string\"}"), mcp_flexible_output_schema(array_schema("hubs") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("architecture_communities", "Group files or symbols into architectural communities.", mcp_tool_schema(level_property + ",\"top\":{\"type\":\"integer\"},\"seed\":{\"type\":\"string\"}"), mcp_flexible_output_schema(array_schema("communities") + ",\"truncated\":{\"type\":\"boolean\"}")),
        mcp_tool_definition("schema_info", "Return RepoLens schema and row-count information.", mcp_empty_tool_schema(), mcp_flexible_output_schema(array_schema("tables") + ",\"row_counts\":{\"type\":\"object\"},\"schema_version\":{\"type\":\"integer\"}")),
        mcp_tool_definition("status", "Return repository index status.", mcp_empty_tool_schema(), mcp_flexible_output_schema("\"repo_root\":{\"type\":\"string\"},\"index_root\":{\"type\":\"string\"},\"database\":{\"type\":\"string\"},\"schema_version\":{\"type\":\"integer\"}")),
        mcp_tool_definition("quality", "Return deterministic quality and grounding measurements.", mcp_tool_schema("\"max_function_lines\":{\"type\":\"integer\"},\"max_file_lines\":{\"type\":\"integer\"},\"complexity_threshold\":{\"type\":\"integer\"},\"top\":{\"type\":\"integer\"}"), mcp_flexible_output_schema("\"summary\":{\"type\":\"object\"},\"thresholds\":{\"type\":\"object\"},\"truncated\":{\"type\":\"boolean\"}"))
    };

    std::ostringstream output;
    output << "{\"tools\":[";
    for (std::size_t index = 0; index < tools.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << tools[index];
    }
    output << "]}";
    return output.str();
}
std::string schema_info_json(const std::filesystem::path& index_dir)
{
    repolens::SqliteDatabase database{database_path_from_index(index_dir)};
    database.create_schema();
    const auto counts = database.count_rows();
    std::ostringstream output;
    output << "{\n";
    output << "  \"schema_version\": " << schema_version << ",\n";
    output << "  \"tables\": [\"repositories\", \"files\", \"symbols\", \"symbol_parameters\", \"symbol_relations\", \"symbol_references\", \"context_descriptions\", \"virtual_files\", \"scip_imports\", \"folder_fingerprints\", \"snapshots\", \"changes\"],\n";
    output << "  \"row_counts\": {\n";
    output << "    \"repositories\": " << counts.repositories << ",\n";
    output << "    \"files\": " << counts.files << ",\n";
    output << "    \"symbols\": " << counts.symbols << ",\n";
    output << "    \"symbol_parameters\": " << counts.symbol_parameters << ",\n";
    output << "    \"symbol_relations\": " << counts.symbol_relations << ",\n";
    output << "    \"symbol_references\": " << counts.symbol_references << ",\n";
    output << "    \"context_descriptions\": " << counts.context_descriptions << ",\n";
    output << "    \"virtual_files\": " << counts.virtual_files << ",\n";
    output << "    \"scip_imports\": " << counts.scip_imports << ",\n";
    output << "    \"snapshots\": " << counts.snapshots << ",\n";
    output << "    \"changes\": " << counts.changes << "\n";
    output << "  },\n";
    output << "  \"mcp_tools\": [\"resolve_symbol\", \"get_snippet\", \"compact_view\", \"search\", \"refs\", \"trace\", \"impact\", \"architecture_hubs\", \"architecture_communities\", \"schema_info\", \"status\", \"quality\"]\n";
    output << "}\n";
    return output.str();
}

std::string mcp_call_tool(const std::filesystem::path& index_dir, const std::string& tool_name, const std::string& arguments)
{
    if (tool_name == "resolve_symbol") {
        const auto name = required_string_argument(arguments, {"name", "symbol"}, "resolve_symbol requires a non-empty name or symbol argument.");
        return fact_symbols_json(resolve_fact_symbols(index_dir, name, optional_string_argument(arguments, "file")));
    }

    if (tool_name == "get_snippet") {
        const auto file = required_string_argument(arguments, {"file"}, "get_snippet requires a non-empty file argument.");
        const int start = json_int_value(arguments, "start", json_int_value(arguments, "line_start", 0));
        const int end = json_int_value(arguments, "end", json_int_value(arguments, "line_end", 0));
        if (start <= 0 || end <= 0) {
            throw std::runtime_error("get_snippet requires positive start and end line arguments.");
        }
        if (end < start) {
            throw std::runtime_error("get_snippet end line must be greater than or equal to start line.");
        }
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        return snippet_json(database.read_snippet(status.repository_id, file, start, end));
    }

    if (tool_name == "compact_view") {
        CompactOptions options;
        options.max_depth = std::max(1, json_int_value(arguments, "max_depth", 8));
        options.budget_chars = std::max(0, json_int_value(arguments, "budget_chars", 12000));
        options.include_private = json_bool_value(arguments, "include_private", false);
        options.json = true;
        std::vector<CompactView> views;
        const auto file = optional_string_argument(arguments, "file");
        const auto symbol = optional_string_argument(arguments, "symbol");
        if (file && !file->empty()) {
            views.push_back(load_compact_view_for_file(index_dir, *file, options));
        } else if (symbol && !symbol->empty()) {
            views = compact_views_for_symbol(index_dir, *symbol, options);
        } else {
            throw std::runtime_error("compact_view requires a non-empty file or symbol argument.");
        }
        return compact_view_json(views, options);
    }

    if (tool_name == "search") {
        repolens::SearchOptions options;
        options.query = required_string_argument(arguments, {"query"}, "search requires a non-empty query argument.");
        options.kind = optional_string_argument(arguments, "kind").value_or("");
        options.limit = json_int_value(arguments, "limit", 20);
        return search_json(search_index(index_dir, options));
    }

    if (tool_name == "refs") {
        const auto symbol = required_string_argument(arguments, {"symbol", "name"}, "refs requires a non-empty symbol argument.");
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        return symbol_references_json(database.references_for_symbol(status.repository_id, symbol));
    }

    if (tool_name == "trace") {
        const auto symbol = required_string_argument(arguments, {"symbol", "name"}, "trace requires a non-empty symbol argument.");
        TraceOptions options;
        options.direction = optional_string_argument(arguments, "direction").value_or("callees");
        if (options.direction != "callees" && options.direction != "callers" && options.direction != "both") {
            throw std::runtime_error("trace direction must be callees, callers, or both.");
        }
        options.max_depth = std::max(0, json_int_value(arguments, "depth", 2));
        options.max_results = std::max(1, json_int_value(arguments, "max_results", 100));
        options.budget_chars = std::max(0, json_int_value(arguments, "budget_chars", 12000));
        options.min_confidence = std::max(0.0, json_double_value(arguments, "min_confidence", 0.0));
        options.json = true;
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        return trace_json(trace_graph(database, status.repository_id, symbol, options), options);
    }

    if (tool_name == "impact") {
        const auto file = optional_string_argument(arguments, "file");
        const auto symbol = optional_string_argument(arguments, "symbol").value_or(optional_string_argument(arguments, "name").value_or(""));
        if ((!file || file->empty()) && symbol.empty()) {
            throw std::runtime_error("impact requires a non-empty symbol or file argument.");
        }
        ImpactOptions options;
        options.max_depth = std::max(0, json_int_value(arguments, "depth", 2));
        options.max_results = std::max(1, json_int_value(arguments, "max_results", 100));
        options.budget_chars = std::max(0, json_int_value(arguments, "budget_chars", 12000));
        options.include_paths = json_bool_value(arguments, "include_paths", false);
        options.json = true;
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        const auto root_target = file ? *file : symbol;
        return impact_json(analyze_impact(database, status.repository_id, root_target, file, options), options);
    }

    if (tool_name == "architecture_hubs" || tool_name == "architecture_communities") {
        ArchitectureOptions options;
        options.hubs = tool_name == "architecture_hubs";
        options.communities = tool_name == "architecture_communities";
        options.level = optional_string_argument(arguments, "level").value_or("file");
        if (options.level != "file" && options.level != "symbol") {
            throw std::runtime_error("architecture level must be file or symbol.");
        }
        options.top = std::max(1, json_int_value(arguments, "top", 20));
        options.seed = optional_string_argument(arguments, "seed").value_or("");
        options.json = true;
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        auto result = build_architecture_graph(database.architecture_edges(status.repository_id), options);
        compute_architecture_communities(result);
        compute_architecture_pagerank(result);
        return architecture_json(result);
    }

    if (tool_name == "schema_info") {
        reject_unexpected_arguments(tool_name, arguments, {});
        return schema_info_json(index_dir);
    }

    if (tool_name == "status") {
        reject_unexpected_arguments(tool_name, arguments, {});
        return status_json(index_dir);
    }

    if (tool_name == "quality") {
        QualityOptions options;
        options.max_function_lines = std::max(1, json_int_value(arguments, "max_function_lines", 200));
        options.max_file_lines = std::max(1, json_int_value(arguments, "max_file_lines", 1000));
        options.complexity_threshold = std::max(1, json_int_value(arguments, "complexity_threshold", 10));
        options.top = std::max(1, json_int_value(arguments, "top", 30));
        repolens::SqliteDatabase database{database_path_from_index(index_dir)};
        database.create_schema();
        const auto status = read_required_status(database);
        return quality_json(build_quality_report(database, status.repository_id, options));
    }

    throw std::runtime_error("Unknown MCP tool: " + tool_name);
}

enum class McpSessionState {
    NotInitialized,
    InitializeResponded,
    Ready,
    ShuttingDown
};

struct McpSession {
    McpSessionState state = McpSessionState::NotInitialized;
    std::string protocol_version;
};

const std::vector<std::string>& mcp_supported_protocol_versions()
{
    static const std::vector<std::string> versions{
        "2025-11-25",
        "2025-06-18",
        "2025-03-26",
        "2024-11-05"
    };
    return versions;
}

std::string mcp_newest_supported_protocol_version()
{
    return mcp_supported_protocol_versions().front();
}

bool mcp_supports_protocol_version(const std::string& version)
{
    const auto& versions = mcp_supported_protocol_versions();
    return std::find(versions.begin(), versions.end(), version) != versions.end();
}

std::string mcp_negotiate_protocol_version(const std::string& requested_version)
{
    if (!requested_version.empty() && mcp_supports_protocol_version(requested_version)) {
        return requested_version;
    }
    return mcp_newest_supported_protocol_version();
}

std::string mcp_not_ready_error(const std::string& id)
{
    return mcp_json_rpc_error(id, -32002, "MCP session is not ready. Send initialize, then notifications/initialized before calling tools.");
}

void handle_mcp_notification(McpSession& session, const std::string& method)
{
    if (method == "notifications/initialized") {
        if (session.state == McpSessionState::InitializeResponded || session.state == McpSessionState::Ready) {
            session.state = McpSessionState::Ready;
        }
        return;
    }

    if (method == "notifications/cancelled" || method == "notifications/progress" || method == "notifications/roots/list_changed") {
        return;
    }

    if (method == "exit") {
        session.state = McpSessionState::ShuttingDown;
        return;
    }
}

std::string mcp_initialize_result(const std::string& protocol_version)
{
    return "{\"protocolVersion\":\"" + json_escape(protocol_version) +
        "\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"RepoLens\",\"version\":\"" +
        std::string{repolens::version} + "\"}}";
}

std::string validate_mcp_envelope(const std::string& request_line, std::string& id, std::string& method)
{
    if (!json_text_looks_like_object(request_line)) {
        return mcp_json_rpc_error("null", -32700, "Invalid JSON.");
    }

    const auto id_raw = json_top_level_raw_value(request_line, "id");
    if (!id_raw) {
        return mcp_json_rpc_error("null", -32600, "Invalid JSON-RPC request: id is required for requests.");
    }

    if (!json_raw_is_valid_id(*id_raw)) {
        id = "null";
        return mcp_json_rpc_error(id, -32600, "Invalid JSON-RPC request id.");
    }
    id = *id_raw;

    const auto jsonrpc_raw = json_top_level_raw_value(request_line, "jsonrpc");
    if (!jsonrpc_raw || !json_raw_is_string(*jsonrpc_raw) || json_string_value(request_line, "jsonrpc").value_or("") != "2.0") {
        return mcp_json_rpc_error(id, -32600, "Invalid JSON-RPC request: jsonrpc must be \"2.0\".");
    }

    const auto method_raw = json_top_level_raw_value(request_line, "method");
    if (!method_raw || !json_raw_is_string(*method_raw)) {
        return mcp_json_rpc_error(id, -32600, "Invalid JSON-RPC request: method must be a string.");
    }
    method = json_string_value(request_line, "method").value_or("");
    if (method.empty()) {
        return mcp_json_rpc_error(id, -32600, "Invalid JSON-RPC request: method is required.");
    }

    const auto params_raw = json_top_level_raw_value(request_line, "params");
    if (params_raw && !json_raw_is_object(*params_raw)) {
        return mcp_json_rpc_error(id, -32602, "Invalid params: params must be an object.");
    }

    return {};
}

std::string handle_mcp_request(const std::filesystem::path& index_dir, const std::string& request_line, McpSession& session)
{
    std::string response_id = "null";
    try {
        if (!json_text_looks_like_object(request_line)) {
            return mcp_json_rpc_error("null", -32700, "Invalid JSON.");
        }

        const auto method_raw_for_routing = json_top_level_raw_value(request_line, "method");
        if (!method_raw_for_routing && (json_top_level_raw_value(request_line, "result") || json_top_level_raw_value(request_line, "error"))) {
            return {};
        }

        const bool has_id = json_top_level_raw_value(request_line, "id").has_value();
        if (!has_id) {
            const auto method_raw = json_top_level_raw_value(request_line, "method");
            const auto method = method_raw && json_raw_is_string(*method_raw) ? json_string_value(request_line, "method").value_or("") : std::string{};
            handle_mcp_notification(session, method);
            return {};
        }

        std::string id = "null";
        std::string method;
        if (const auto error = validate_mcp_envelope(request_line, id, method); !error.empty()) {
            return error;
        }
        response_id = id;

        if (method == "ping") {
            return mcp_json_rpc_result(id, "{}");
        }

        if (method == "shutdown") {
            session.state = McpSessionState::ShuttingDown;
            return mcp_json_rpc_result(id, "{}");
        }

        if (method == "initialize") {
            if (session.state != McpSessionState::NotInitialized) {
                return mcp_json_rpc_error(id, -32002, "MCP session is already initialized.");
            }
            const auto params = json_object_value(request_line, "params").value_or("{}");
            session.protocol_version = mcp_negotiate_protocol_version(json_string_value(params, "protocolVersion").value_or(""));
            session.state = McpSessionState::InitializeResponded;
            return mcp_json_rpc_result(id, mcp_initialize_result(session.protocol_version));
        }

        if (method == "tools/list") {
            if (session.state != McpSessionState::Ready) {
                return mcp_not_ready_error(id);
            }
            const auto params_raw = json_top_level_raw_value(request_line, "params");
            if (params_raw) {
                const auto cursor_raw = json_top_level_raw_value(*params_raw, "cursor");
                if (cursor_raw) {
                    if (!json_raw_is_string(*cursor_raw)) {
                        return mcp_json_rpc_error(id, -32602, "tools/list cursor must be a string when provided.");
                    }
                    if (!json_string_value(*params_raw, "cursor").value_or("").empty()) {
                        return mcp_json_rpc_error(id, -32602, "RepoLens tools/list returns a static full list and does not support pagination cursors.");
                    }
                }
            }
            return mcp_json_rpc_result(id, mcp_tools_list_json());
        }
        if (method == "tools/call") {
            if (session.state != McpSessionState::Ready) {
                return mcp_not_ready_error(id);
            }
            const auto params_raw = json_top_level_raw_value(request_line, "params");
            if (!params_raw || !json_raw_is_object(*params_raw)) {
                return mcp_json_rpc_error(id, -32602, "tools/call requires object params.");
            }
            const auto params = *params_raw;
            const auto tool_name_raw = json_top_level_raw_value(params, "name");
            if (!tool_name_raw || !json_raw_is_string(*tool_name_raw)) {
                return mcp_json_rpc_error(id, -32602, "tools/call requires string params.name.");
            }
            const auto tool_name = json_string_value(params, "name").value_or("");
            if (tool_name.empty()) {
                return mcp_json_rpc_error(id, -32602, "tools/call requires non-empty params.name.");
            }
            if (!mcp_tool_exists(tool_name)) {
                return mcp_json_rpc_error(id, -32602, "Unknown MCP tool: " + tool_name);
            }
            const auto arguments_raw = json_top_level_raw_value(params, "arguments");
            if (arguments_raw && !json_raw_is_object(*arguments_raw)) {
                return mcp_json_rpc_error(id, -32602, "tools/call params.arguments must be an object.");
            }
            const auto arguments = arguments_raw.value_or("{}");
            try {
                return mcp_json_rpc_result(id, mcp_content_result(tool_name, mcp_call_tool(index_dir, tool_name, arguments)));
            } catch (const std::exception& tool_error) {
                return mcp_json_rpc_result(id, mcp_tool_error_result(tool_error.what()));
            }
        }
        return mcp_json_rpc_error(id, -32601, "Unknown JSON-RPC method: " + method);
    } catch (const std::exception& error) {
        return mcp_json_rpc_error(response_id, -32603, error.what());
    }
}

int run_mcp_server(const std::filesystem::path& index_dir)
{
    (void)database_path_from_index(index_dir);
    McpSession session;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        const auto response = handle_mcp_request(index_dir, line, session);
        if (!response.empty()) {
            std::cout << response;
            std::cout.flush();
        }
    }
    return 0;
}
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket_handle = INVALID_SOCKET;
void close_socket(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
constexpr SocketHandle invalid_socket_handle = -1;
void close_socket(SocketHandle socket) { close(socket); }
#endif

std::string http_response(int status_code, const std::string& status_text, const std::string& body)
{
    std::ostringstream response;
    response
        << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return response.str();
}

std::string error_json(const std::string& message)
{
    return "{\n  \"error\": \"" + json_escape(message) + "\"\n}\n";
}

HttpRequest read_http_request(SocketHandle client)
{
    std::string data;
    char buffer[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        data.append(buffer, buffer + received);
        if (data.size() > 1024 * 1024) {
            throw std::runtime_error("HTTP request is too large.");
        }
    }

    const auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("Invalid HTTP request.");
    }

    const std::string headers = data.substr(0, header_end);
    std::istringstream request_line_stream{headers};
    HttpRequest request;
    request_line_stream >> request.method >> request.path;

    std::size_t content_length = 0;
    std::istringstream header_stream{headers};
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string prefix = "Content-Length:";
        if (line.rfind(prefix, 0) == 0) {
            content_length = static_cast<std::size_t>(std::stoul(line.substr(prefix.size())));
        }
    }

    request.body = data.substr(header_end + 4);
    while (request.body.size() < content_length) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        request.body.append(buffer, buffer + received);
    }
    if (request.body.size() > content_length) {
        request.body.resize(content_length);
    }

    return request;
}

std::string handle_http_request(const std::filesystem::path& index_dir, const HttpRequest& request)
{
    try {
        if (request.method == "GET" && request.path == "/health") {
            return http_response(200, "OK", "{\n  \"status\": \"ok\",\n  \"service\": \"RepoLens\"\n}\n");
        }

        if (request.method == "GET" && request.path == "/status") {
            return http_response(200, "OK", status_json(index_dir));
        }

        if (request.method == "POST" && request.path == "/update") {
            NullProgressReporter reporter;
            return http_response(200, "OK", update_json(update_index(index_dir, reporter, {}, json_bool_value(request.body, "lite", false))));
        }

        if (request.method == "POST" && request.path == "/search") {
            repolens::SearchOptions options;
            options.query = json_string_value(request.body, "query").value_or("");
            options.kind = json_string_value(request.body, "kind").value_or("");
            options.limit = json_int_value(request.body, "limit", 20);
            if (options.query.empty()) {
                return http_response(400, "Bad Request", error_json("Missing search query."));
            }
            return http_response(200, "OK", search_json(search_index(index_dir, options)));
        }

        if (request.method == "POST" && request.path == "/context") {
            const auto symbols = json_string_array_value(request.body, "symbols");
            if (symbols.empty()) {
                return http_response(400, "Bad Request", error_json("Missing context symbols."));
            }
            const int budget_chars = json_int_value(request.body, "budget_chars", 60000);
            const bool include_tree = json_bool_value(request.body, "include_tree", false);
            const bool include_types = json_bool_value(request.body, "include_types", false);
            const bool partial = json_bool_value(request.body, "partial", false);
            const bool basic = json_bool_value(request.body, "basic", false);
            const int level = std::max(0, json_int_value(request.body, "level", 0));
            if (basic) {
                return http_response(200, "OK", context_basic_json(index_dir, symbols, partial, level));
            }
            return http_response(200, "OK", context_json(index_dir, symbols, budget_chars, include_tree, include_types, partial, level));
        }

        return http_response(404, "Not Found", error_json("Endpoint not found."));
    } catch (const std::exception& error) {
        return http_response(500, "Internal Server Error", error_json(error.what()));
    }
}

int run_serve(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens serve --index-dir <index_path> [--port 7123|--mcp]");
    }
    if (has_flag(argc, argv, "--mcp")) {
        return run_mcp_server(*index_dir);
    }
#if !REPOLENS_ENABLE_API
    throw std::runtime_error("RepoLens was built with REPOLENS_ENABLE_API=OFF.");
#else
    const int port = read_option_int(argc, argv, "--port", 7123);
    (void)database_path_from_index(*index_dir);

#if defined(_WIN32)
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("Failed to initialize WinSock.");
    }
#endif

    SocketHandle server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == invalid_socket_handle) {
        throw std::runtime_error("Failed to create server socket.");
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(server);
        throw std::runtime_error("Failed to bind server to 127.0.0.1.");
    }

    if (listen(server, 16) != 0) {
        close_socket(server);
        throw std::runtime_error("Failed to listen for HTTP connections.");
    }

    std::cout << "RepoLens API listening on http://127.0.0.1:" << port << '\n';

    while (true) {
        SocketHandle client = accept(server, nullptr, nullptr);
        if (client == invalid_socket_handle) {
            continue;
        }

        try {
            const auto request = read_http_request(client);
            const auto response = handle_http_request(*index_dir, request);
            send(client, response.c_str(), static_cast<int>(response.size()), 0);
        } catch (const std::exception& error) {
            const auto response = http_response(400, "Bad Request", error_json(error.what()));
            send(client, response.c_str(), static_cast<int>(response.size()), 0);
        }

        close_socket(client);
    }

    close_socket(server);
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
#endif
}

struct AiConfig {
    bool enabled = false;
    std::string provider = "none";
    std::string endpoint;
    std::string model;
    std::string api_key_env;
};

struct EnrichSummary {
    int considered = 0;
    int enriched = 0;
    int skipped = 0;
    int failed = 0;
};

AiConfig load_ai_config(const std::filesystem::path& index_dir)
{
    const auto config_path = canonical_existing_directory(index_dir, "index_dir") / "config.json";
    if (!std::filesystem::exists(config_path)) {
        return {};
    }

    std::ifstream file{config_path};
    if (!file) {
        throw std::runtime_error("Failed to read AI config: " + config_path.string());
    }

    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    AiConfig config;
    config.enabled = json_bool_value(text, "enabled", false);
    config.provider = json_string_value(text, "provider").value_or("none");
    config.endpoint = json_string_value(text, "endpoint").value_or("");
    config.model = json_string_value(text, "model").value_or("");
    config.api_key_env = json_string_value(text, "api_key_env").value_or("");
    return config;
}

class IAiProvider {
public:
    virtual ~IAiProvider() = default;
    virtual bool enabled() const = 0;
    virtual std::optional<repolens::EnrichmentResult> enrich(const repolens::SymbolForEnrichment& symbol) = 0;
};

class NullAiProvider final : public IAiProvider {
public:
    bool enabled() const override
    {
        return false;
    }

    std::optional<repolens::EnrichmentResult> enrich(const repolens::SymbolForEnrichment&) override
    {
        return std::nullopt;
    }
};

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
};

ParsedUrl parse_http_url(const std::string& endpoint)
{
    const std::string prefix = "http://";
    if (endpoint.rfind(prefix, 0) != 0) {
        throw std::runtime_error("Only http:// AI endpoints are supported in this build.");
    }

    ParsedUrl url;
    auto rest = endpoint.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        url.path = rest.substr(slash);
        rest = rest.substr(0, slash);
    }

    const auto colon = rest.find(':');
    if (colon != std::string::npos) {
        url.host = rest.substr(0, colon);
        url.port = std::stoi(rest.substr(colon + 1));
    } else {
        url.host = rest;
    }

    if (url.host.empty()) {
        throw std::runtime_error("AI endpoint host is empty.");
    }
    return url;
}

std::string http_post_json(const std::string& endpoint, const std::string& body, const std::string& api_key)
{
    const auto url = parse_http_url(endpoint);

#if defined(_WIN32)
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        throw std::runtime_error("Failed to initialize WinSock.");
    }
#endif

    SocketHandle client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == invalid_socket_handle) {
        throw std::runtime_error("Failed to create AI HTTP socket.");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(url.port));
    if (inet_pton(AF_INET, url.host.c_str(), &address.sin_addr) != 1) {
        close_socket(client);
        throw std::runtime_error("AI endpoint host must be an IPv4 address.");
    }

    if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(client);
        throw std::runtime_error("Failed to connect to AI endpoint.");
    }

    std::ostringstream request;
    request
        << "POST " << url.path << " HTTP/1.1\r\n"
        << "Host: " << url.host << ':' << url.port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    if (!api_key.empty()) {
        request << "Authorization: Bearer " << api_key << "\r\n";
    }
    request << "Connection: close\r\n\r\n" << body;

    const auto request_text = request.str();
    send(client, request_text.c_str(), static_cast<int>(request_text.size()), 0);

    std::string response;
    char buffer[4096];
    while (true) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, buffer + received);
    }
    close_socket(client);
#if defined(_WIN32)
    WSACleanup();
#endif

    const auto body_start = response.find("\r\n\r\n");
    return body_start == std::string::npos ? response : response.substr(body_start + 4);
}

std::string tags_to_csv(const std::vector<std::string>& tags)
{
    std::string result;
    for (std::size_t index = 0; index < tags.size(); ++index) {
        if (index > 0) {
            result += ",";
        }
        result += tags[index];
    }
    return result;
}

class OpenAICompatibleProvider final : public IAiProvider {
public:
    explicit OpenAICompatibleProvider(AiConfig config)
        : config_(std::move(config))
    {
        if (!config_.api_key_env.empty()) {
            if (const char* value = std::getenv(config_.api_key_env.c_str())) {
                api_key_ = value;
            }
        }
    }

    bool enabled() const override
    {
        return config_.enabled;
    }

    std::optional<repolens::EnrichmentResult> enrich(const repolens::SymbolForEnrichment& symbol) override
    {
        std::ostringstream user_prompt;
        user_prompt
            << "Describe this code symbol briefly and return JSON with description and tags.\n"
            << "Kind: " << symbol.kind << "\n"
            << "Name: " << symbol.qualified_name << "\n"
            << "Signature: " << symbol.signature << "\n"
            << "File: " << symbol.file_path << ':' << symbol.line_start << "\n";

        std::ostringstream body;
        body
            << "{"
            << "\"model\":\"" << json_escape(config_.model) << "\","
            << "\"messages\":["
            << "{\"role\":\"system\",\"content\":\"Return only compact JSON with description and tags.\"},"
            << "{\"role\":\"user\",\"content\":\"" << json_escape(user_prompt.str()) << "\"}"
            << "],"
            << "\"temperature\":0"
            << "}";

        const auto response = http_post_json(config_.endpoint, body.str(), api_key_);
        const auto content = json_string_value(response, "content").value_or(response);
        const auto description = json_string_value(content, "description");
        auto tags = json_string_array_value(content, "tags");
        if (tags.empty()) {
            const auto tag_string = json_string_value(content, "tags");
            if (tag_string) {
                tags = split_symbols(*tag_string);
            }
        }

        if (!description || description->empty()) {
            return std::nullopt;
        }

        repolens::EnrichmentResult result;
        result.description = *description;
        result.tags = tags_to_csv(tags);
        result.ai_description = result.description;
        result.ai_tags = result.tags;
        result.ai_model = config_.model;
        return result;
    }

private:
    AiConfig config_;
    std::string api_key_;
};

std::unique_ptr<IAiProvider> create_ai_provider(const AiConfig& config)
{
    if (!config.enabled) {
        return std::make_unique<NullAiProvider>();
    }
    if (config.provider != "openai-compatible") {
        throw std::runtime_error("Unsupported AI provider: " + config.provider);
    }
    if (config.endpoint.empty()) {
        throw std::runtime_error("AI endpoint is required when AI is enabled.");
    }
    return std::make_unique<OpenAICompatibleProvider>(config);
}

EnrichSummary enrich_index(const std::filesystem::path& index_dir, bool changed_only)
{
    const auto config = load_ai_config(index_dir);
    auto provider = create_ai_provider(config);
    EnrichSummary summary;

    if (!provider->enabled()) {
        return summary;
    }

    const auto database_path = database_path_from_index(index_dir);
    repolens::SqliteDatabase database{database_path};
    database.create_schema();
    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    const auto symbols = database.symbols_for_enrichment(status->repository_id, changed_only);
    summary.considered = static_cast<int>(symbols.size());
    for (const auto& symbol : symbols) {
        const auto result = provider->enrich(symbol);
        if (!result) {
            ++summary.failed;
            continue;
        }
        database.update_symbol_enrichment(symbol.symbol_id, *result);
        ++summary.enriched;
    }

    return summary;
}

int run_enrich(int argc, char* argv[])
{
#if !REPOLENS_ENABLE_AI
    (void)argc;
    (void)argv;
    throw std::runtime_error("RepoLens was built with REPOLENS_ENABLE_AI=OFF.");
#else
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens enrich --index-dir <index_path> [--changed-only]");
    }

    const auto summary = enrich_index(*index_dir, has_flag(argc, argv, "--changed-only"));
    std::cout
        << "Enrichment complete\n"
        << "Considered: " << summary.considered << '\n'
        << "Enriched: " << summary.enriched << '\n'
        << "Skipped: " << summary.skipped << '\n'
        << "Failed: " << summary.failed << '\n';
    return 0;
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 1) {
            print_help();
            return 0;
        }

        const std::string_view command{argv[1]};

        if (argc == 2) {
            if (command == "--help") {
                print_help();
                return 0;
            }

            if (command == "--version") {
                print_version();
                return 0;
            }
        }

        if (command == "init") {
            return run_init(argc, argv);
        }

        if (command == "status") {
            return run_status(argc, argv);
        }

        if (command == "update") {
            return run_update(argc, argv);
        }

        if (command == "updateroot") {
            return run_updateroot(argc, argv);
        }

        if (command == "update-files") {
            return run_update_files(argc, argv);
        }

        if (command == "diagnostics") {
            return run_diagnostics(argc, argv);
        }

        if (command == "compact") {
            return run_compact(argc, argv);
        }

        if (command == "import-scip") {
            return run_import_scip(argc, argv);
        }

        if (command == "search") {
            return run_search(argc, argv);
        }

        if (command == "signals") {
            return run_signals(argc, argv);
        }

        if (command == "resolve-symbol") {
            return run_resolve_symbol(argc, argv);
        }

        if (command == "snippet") {
            return run_snippet(argc, argv);
        }

        if (command == "symbol-range") {
            return run_symbol_range(argc, argv);
        }

        if (command == "compact-view") {
            return run_compact_view(argc, argv);
        }

        if (command == "describe") {
            return run_describe(argc, argv);
        }

        if (command == "refs") {
            return run_refs(argc, argv);
        }

        if (command == "relationships") {
            return run_relationships(argc, argv);
        }

        if (command == "unresolved-refs") {
            return run_unresolved_refs(argc, argv);
        }

        if (command == "trace") {
            return run_trace(argc, argv);
        }

        if (command == "architecture") {
            return run_architecture(argc, argv);
        }

        if (command == "impact") {
            return run_impact(argc, argv);
        }

        if (command == "quality") {
            return run_quality(argc, argv);
        }

        if (command == "context") {
            return run_context(argc, argv);
        }

        if (command == "direct-context") {
            return run_direct_context(argc, argv);
        }

        if (command == "serve") {
            return run_serve(argc, argv);
        }

        if (command == "enrich") {
            return run_enrich(argc, argv);
        }

        std::cerr << "Unknown or invalid arguments. Use --help for usage.\n";
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }

    return 1;
}





























