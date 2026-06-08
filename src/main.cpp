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
        << "  status --index-dir <index_path>              Show index status.\n"
        << "  update --index-dir <index_path> [--format text|json] [--quiet] [--verbose] [--no-progress]\n"
        << "                                               Scan files and update the index.\n"
        << "  updateroot [--include-file <path>] [--exclude-file <path>]\n"
        << "                                               Update repolens.db beside the executable using path lists.\n"
        << "  diagnostics --index-dir <index_path>         Show SQLite row counts and database size.\n"
        << "  search --index-dir <index_path> --query <text> [--kind <kind>] [--limit <n>] [--partial] [--format text|json]\n"
        << "  context --index-dir <index_path> --symbols <A,B> [--partial] [--basic] [--level <n>] [--budget-chars <n>] [--include-tree] [--include-types] --format json\n"
        << "  serve --index-dir <index_path> [--port 7123] Start local HTTP API on 127.0.0.1.\n"
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
            << "Database:         " << summary.database_path << "\n\n"
            << "Started at:       " << summary.started_at << '\n'
            << "Finished at:      " << summary.finished_at << '\n'
            << "Elapsed time:     " << std::fixed << std::setprecision(2) << summary.elapsed_seconds << " seconds\n\n"
            << "Files:\n"
            << "Discovered:       " << summary.discovered << '\n'
            << "Ignored:          " << summary.ignored << '\n'
            << "Processed:        " << summary.processed << '\n'
            << "Source bytes:     " << summary.source_bytes << '\n'
            << "Added:            " << summary.added << '\n'
            << "Modified:         " << summary.modified << '\n'
            << "Deleted:          " << summary.deleted << '\n'
            << "Unchanged:        " << summary.unchanged << '\n'
            << "Failed:           " << summary.failed << "\n\n"
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
    UpdateSummary& summary)
{
    const auto* interpreter = interpreters.find_for_file(file);
    if (!interpreter) {
        ++counts.skipped;
        return;
    }

    try {
        const auto result = interpreter->parse_file(file);
        if (result.success) {
            const auto stats = database.save_parse_result(repository_id, file_id, result);
            summary.symbols_added += stats.symbols_inserted;
            summary.symbols_updated += std::min(stats.symbols_inserted, stats.symbols_deleted);
            summary.symbols_deleted += std::max(0, stats.symbols_deleted - stats.symbols_inserted);
            summary.symbols_deactivated += stats.symbols_deactivated;
            ++counts.parsed;
        } else {
            const auto stats = database.save_parse_result(repository_id, file_id, result);
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
    const repolens::ScanOptions& scan_options);

UpdateSummary update_index(const std::filesystem::path& index_dir, UpdateProgressReporter& reporter)
{
    return update_index(index_dir, reporter, {});
}

UpdateSummary update_index(
    const std::filesystem::path& index_dir,
    UpdateProgressReporter& reporter,
    const repolens::ScanOptions& scan_options)
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

    repolens::SqliteDatabase database{database_path};
    database.create_schema();

    const auto status = database.read_repository_status();
    if (!status) {
        throw std::runtime_error("Repository metadata was not found. Run init first.");
    }

    const auto repo_root = canonical_existing_directory(status->repo_root, "repo_root");
    validate_external_index(repo_root, index_root);
    summary.repo_root = repo_root.string();

    reporter.on_scan_started(summary);

    const auto scan_result = repolens::scan_repository(repo_root, scan_options);
    const auto& scanned_files = scan_result.files;
    summary.discovered = scan_result.discovered_count;
    summary.ignored = scan_result.ignored_count;
    reporter.on_scan_completed(summary);

    auto stored_files = database.read_files(status->repository_id);
    summary.symbols_before = database.count_active_symbols(status->repository_id);

    std::unordered_set<std::string> seen_paths;
    repolens::InterpreterRegistry interpreters;
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
    auto ensure_snapshot = [&]() {
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
                database.upsert_file(status->repository_id, scanned_file, snapshot_id, true);
                const auto refreshed = database.read_files(status->repository_id).at(scanned_file.relative_path);
                database.record_change(
                    status->repository_id,
                    snapshot_id,
                    "file",
                    refreshed.id,
                    "file_added",
                    "",
                    scanned_file.content_hash,
                    "",
                    scanned_file.relative_path);
                parse_changed_file(database, interpreters, status->repository_id, refreshed.id, scanned_file, summary.parse_counts, summary);
                ++summary.added;
            } else if (!stored->second.is_active) {
                const auto snapshot_id = ensure_snapshot();
                database.upsert_file(status->repository_id, scanned_file, snapshot_id, false);
                database.record_change(
                    status->repository_id,
                    snapshot_id,
                    "file",
                    stored->second.id,
                    "file_added",
                    "",
                    scanned_file.content_hash,
                    "",
                    scanned_file.relative_path);
                parse_changed_file(database, interpreters, status->repository_id, stored->second.id, scanned_file, summary.parse_counts, summary);
                ++summary.added;
            } else if (stored->second.content_hash != scanned_file.content_hash) {
                const auto snapshot_id = ensure_snapshot();
                database.upsert_file(status->repository_id, scanned_file, snapshot_id, false);
                database.record_change(
                    status->repository_id,
                    snapshot_id,
                    "file",
                    stored->second.id,
                    "file_modified",
                    stored->second.content_hash,
                    scanned_file.content_hash,
                    scanned_file.relative_path,
                    scanned_file.relative_path);
                parse_changed_file(database, interpreters, status->repository_id, stored->second.id, scanned_file, summary.parse_counts, summary);
                ++summary.modified;
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

    for (const auto& [relative_path, stored_file] : stored_files) {
        if (!stored_file.is_active || seen_paths.find(relative_path) != seen_paths.end()) {
            continue;
        }

        const auto snapshot_id = ensure_snapshot();
        database.mark_file_deleted(stored_file.id, snapshot_id);
        summary.symbols_deactivated += database.mark_symbols_inactive_for_file(stored_file.id);
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
        ++summary.deleted;
    }

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
    return update_index(index_dir, reporter, {});
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

int run_status(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (argc != 4 || !index_dir) {
        throw std::runtime_error("Usage: repolens status --index-dir <index_path>");
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
        << "Added: " << summary.added << '\n'
        << "Modified: " << summary.modified << '\n'
        << "Deleted: " << summary.deleted << '\n'
        << "Unchanged: " << summary.unchanged << '\n'
        << "Processed: " << summary.processed << '\n'
        << "Source bytes: " << summary.source_bytes << '\n'
        << "Failed: " << summary.failed << '\n'
        << "Symbols inserted: " << summary.symbols_added << '\n'
        << "Symbols updated: " << summary.symbols_updated << '\n'
        << "Symbols deactivated: " << summary.symbols_deactivated << '\n'
        << "Parsed: " << summary.parse_counts.parsed << '\n'
        << "Parse skipped: " << summary.parse_counts.skipped << '\n'
        << "Parse failed: " << summary.parse_counts.failed << '\n';
}

std::string update_json(const UpdateSummary& summary);

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

int run_update(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens update --index-dir <index_path> [--format text|json] [--progress|--no-progress] [--quiet] [--verbose]");
    }

    const auto format = read_option_string(argc, argv, "--format").value_or("text");
    if (format != "text" && format != "json") {
        throw std::runtime_error("--format must be text or json.");
    }

    const bool quiet = has_flag(argc, argv, "--quiet");
    const bool no_progress = quiet || has_flag(argc, argv, "--no-progress") || format == "json";
    const bool verbose = has_flag(argc, argv, "--verbose");

    if (no_progress) {
        NullProgressReporter reporter;
        const auto summary = update_index(*index_dir, reporter);
        if (format == "json") {
            std::cout << update_json(summary);
        } else {
            print_update_compact(summary);
        }
    } else {
        ConsoleProgressReporter reporter{verbose};
        update_index(*index_dir, reporter);
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
        if (argument == "--include-file" || argument == "--exclude-file") {
            ++index;
            if (index >= argc || std::string_view{argv[index]}.rfind("--", 0) == 0) {
                throw std::runtime_error("Usage: repolens updateroot [--include-file <path>] [--exclude-file <path>]");
            }
            continue;
        }

        throw std::runtime_error("Usage: repolens updateroot [--include-file <path>] [--exclude-file <path>]");
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

    ConsoleProgressReporter reporter{false};
    const auto summary = update_index(root, reporter, scan_options);

    std::cout
        << "\nUpdateroot configuration\n"
        << "------------------------\n"
        << "Executable root: " << root.string() << '\n'
        << "Include file:    " << include_file.string() << '\n'
        << "Exclude file:    " << (std::filesystem::exists(exclude_file) ? exclude_file.string() : "(not found)") << '\n'
        << "Included paths:  " << include_paths.size() << '\n'
        << "Excluded paths:  " << exclude_paths.size() << '\n'
        << "Repo root:       " << summary.repo_root << '\n'
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

struct ContextItem {
    std::string requested_symbol;
    repolens::ContextSymbolCandidate symbol;
    std::string code;
    std::string relation_type;
    std::string source_qualified_name;
    int level = 0;
    bool related = false;
    bool truncated = false;
};

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

std::string context_basic_json(
    const std::filesystem::path& index_dir,
    const std::vector<std::string>& requested_symbols,
    bool partial_match,
    int requested_level = 0)
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

std::string context_json(
    const std::filesystem::path& index_dir,
    const std::vector<std::string>& requested_symbols,
    int budget_chars,
    bool include_tree,
    bool include_types,
    bool partial_match,
    int requested_level = 0)
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

    const auto tree_paths = include_tree ? database.active_file_paths(status->repository_id) : std::vector<std::string>{};

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
        output << "      \"code\": \"" << json_escape(item.code) << "\"\n";
        output << "    }";
        if (index + 1 < items.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ],\n";
    output << "  \"reduced_file_tree\": " << string_array_json(tree_paths, "    ") << ",\n";
    output << "  \"warnings\": " << string_array_json(warnings, "    ") << "\n";
    output << "}\n";

    return output.str();
}

int run_context(int argc, char* argv[])
{
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    const auto symbols_option = read_option_string(argc, argv, "--symbols");
    const auto format = read_option_string(argc, argv, "--format").value_or("json");
    if (!index_dir || !symbols_option || symbols_option->empty()) {
        throw std::runtime_error("Usage: repolens context --index-dir <index_path> --symbols <A,B> [--partial] [--basic] [--level <n>] [--budget-chars <n>] [--include-tree] [--include-types] --format json");
    }
    if (format != "json") {
        throw std::runtime_error("Phase 6 context output supports --format json only.");
    }

    const bool partial_match = has_flag(argc, argv, "--partial");
    const int requested_level = std::max(0, read_option_int(argc, argv, "--level", 0));
    if (has_flag(argc, argv, "--basic")) {
        std::cout << context_basic_json(*index_dir, split_symbols(*symbols_option), partial_match, requested_level);
        return 0;
    }

    std::cout << context_json(
        *index_dir,
        split_symbols(*symbols_option),
        read_option_int(argc, argv, "--budget-chars", 60000),
        has_flag(argc, argv, "--include-tree"),
        has_flag(argc, argv, "--include-types"),
        partial_match,
        requested_level);

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
    output << "  \"timing\": {\n";
    output << "    \"started_at\": \"" << json_escape(summary.started_at) << "\",\n";
    output << "    \"finished_at\": \"" << json_escape(summary.finished_at) << "\",\n";
    output << "    \"elapsed_seconds\": " << std::fixed << std::setprecision(3) << summary.elapsed_seconds << "\n";
    output << "  },\n";
    output << "  \"files\": {\n";
    output << "    \"discovered\": " << summary.discovered << ",\n";
    output << "    \"ignored\": " << summary.ignored << ",\n";
    output << "    \"processed\": " << summary.processed << ",\n";
    output << "    \"source_bytes\": " << summary.source_bytes << ",\n";
    output << "    \"added\": " << summary.added << ",\n";
    output << "    \"modified\": " << summary.modified << ",\n";
    output << "    \"deleted\": " << summary.deleted << ",\n";
    output << "    \"unchanged\": " << summary.unchanged << ",\n";
    output << "    \"failed\": " << summary.failed << "\n";
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
    output << "    \"created_at\": \"" << json_escape(summary.finished_at) << "\"\n";
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
        << "snapshots:          " << counts.snapshots << '\n'
        << "changes:            " << counts.changes << '\n';

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
            return http_response(200, "OK", update_json(update_index(index_dir)));
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
#if !REPOLENS_ENABLE_API
    (void)argc;
    (void)argv;
    throw std::runtime_error("RepoLens was built with REPOLENS_ENABLE_API=OFF.");
#else
    const auto index_dir = read_option_path(argc, argv, "--index-dir");
    if (!index_dir) {
        throw std::runtime_error("Usage: repolens serve --index-dir <index_path> [--port 7123]");
    }
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

        if (command == "diagnostics") {
            return run_diagnostics(argc, argv);
        }

        if (command == "search") {
            return run_search(argc, argv);
        }

        if (command == "context") {
            return run_context(argc, argv);
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
