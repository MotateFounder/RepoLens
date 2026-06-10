#include "repolens/file_scanner.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool is_ignored_directory(const std::filesystem::path& path)
{
    static constexpr std::array ignored{".git", "bin", "obj", "node_modules", "build", "dist", ".vs", ".idea"};
    const auto name = path.filename().string();
    return std::find(ignored.begin(), ignored.end(), name) != ignored.end();
}

std::string to_hex(std::uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

struct FileContentStats {
    long long lines = 0;
    long long chars = 0;
    std::string hash;
};

FileContentStats read_content_stats(const std::filesystem::path& path)
{
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read file for metadata: " + path.string());
    }

    FileContentStats stats;
    std::uint64_t hash = offset_basis;
    bool saw_any = false;
    bool ended_with_newline = false;
    char buffer[8192];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        const auto count = file.gcount();
        saw_any = saw_any || count > 0;
        stats.chars += count;

        for (std::streamsize index = 0; index < count; ++index) {
            const auto byte = static_cast<unsigned char>(buffer[index]);
            hash ^= byte;
            hash *= prime;

            ended_with_newline = byte == '\n';
            if (ended_with_newline) {
                ++stats.lines;
            }
        }
    }

    if (saw_any && !ended_with_newline) {
        ++stats.lines;
    }

    stats.hash = to_hex(hash);
    return stats;
}

std::string file_time_to_string(const std::filesystem::file_time_type& value)
{
    const auto ticks = value.time_since_epoch().count();
    return std::to_string(ticks);
}

std::string normalize_relative_path(const std::filesystem::path& path)
{
    return path.generic_string();
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

bool is_excluded_path(const std::filesystem::path& path, const std::vector<std::filesystem::path>& excludes)
{
    for (const auto& exclude : excludes) {
        if (is_same_or_child_path(exclude, path)) {
            return true;
        }
    }
    return false;
}

repolens::FileMetadata read_file_metadata(const std::filesystem::path& repo_root, const std::filesystem::path& path)
{
    const auto absolute_path = std::filesystem::canonical(path);
    const auto relative_path = std::filesystem::relative(absolute_path, repo_root);
    const auto stats = read_content_stats(absolute_path);

    repolens::FileMetadata metadata;
    metadata.relative_path = normalize_relative_path(relative_path);
    metadata.absolute_path = absolute_path.string();
    metadata.extension = absolute_path.extension().string();
    metadata.size_bytes = static_cast<long long>(std::filesystem::file_size(absolute_path));
    metadata.line_count = stats.lines;
    metadata.char_count = stats.chars;
    metadata.last_modified_time = file_time_to_string(std::filesystem::last_write_time(absolute_path));
    metadata.content_hash = stats.hash;
    return metadata;
}

} // namespace

namespace repolens {

RepositoryScanResult scan_repository(const std::filesystem::path& repo_root, const ScanOptions& options)
{
    RepositoryScanResult result;

    const auto root = std::filesystem::canonical(repo_root);
    std::vector<std::filesystem::path> includes;
    std::vector<std::filesystem::path> excludes;

    for (const auto& include : options.include_paths) {
        if (std::filesystem::exists(include)) {
            includes.push_back(std::filesystem::canonical(include));
        }
    }
    for (const auto& exclude : options.exclude_paths) {
        if (std::filesystem::exists(exclude)) {
            excludes.push_back(std::filesystem::canonical(exclude));
        }
    }
    if (includes.empty()) {
        includes.push_back(root);
    }

    std::vector<std::filesystem::path> scan_roots;
    for (const auto& include : includes) {
        if (is_excluded_path(include, excludes)) {
            if (std::filesystem::is_regular_file(include)) {
                ++result.ignored_count;
            } else if (std::filesystem::is_directory(include)) {
                ++result.ignored_count;
            }
            continue;
        }

        bool covered_by_existing_root = false;
        for (const auto& existing : scan_roots) {
            if (is_same_or_child_path(existing, include)) {
                covered_by_existing_root = true;
                break;
            }
        }
        if (!covered_by_existing_root) {
            scan_roots.push_back(include);
        }
    }

    std::set<std::string> seen_files;
    auto add_file = [&](const std::filesystem::path& path) {
        if (is_excluded_path(path, excludes)) {
            ++result.ignored_count;
            return;
        }

        const auto metadata = read_file_metadata(root, path);
        if (seen_files.insert(metadata.relative_path).second) {
            result.files.push_back(metadata);
        }
    };

    for (const auto& scan_root : scan_roots) {
        if (std::filesystem::is_regular_file(scan_root)) {
            add_file(scan_root);
            continue;
        }

        if (!std::filesystem::is_directory(scan_root)) {
            continue;
        }

    std::filesystem::recursive_directory_iterator iterator{
        scan_root,
        std::filesystem::directory_options::skip_permission_denied};
    const std::filesystem::recursive_directory_iterator end;

    for (; iterator != end; ++iterator) {
        const auto& entry = *iterator;

        if (entry.is_directory()) {
            if (is_ignored_directory(entry.path()) || is_excluded_path(entry.path(), excludes)) {
                ++result.ignored_count;
                iterator.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file()) {
            continue;
        }

        add_file(entry.path());
    }
    }

    std::sort(result.files.begin(), result.files.end(), [](const FileMetadata& left, const FileMetadata& right) {
        return left.relative_path < right.relative_path;
    });

    result.discovered_count = static_cast<int>(result.files.size()) + result.ignored_count;
    return result;
}

std::vector<FileMetadata> scan_repository_files(const std::filesystem::path& repo_root)
{
    return scan_repository(repo_root).files;
}

FileMetadata scan_file(const std::filesystem::path& repo_root, const std::filesystem::path& file_path)
{
    const auto root = std::filesystem::canonical(repo_root);
    return read_file_metadata(root, file_path);
}

} // namespace repolens
