#pragma once

#include "repolens/sqlite_database.hpp"

#include <filesystem>
#include <vector>

namespace repolens {

struct RepositoryScanResult {
    std::vector<FileMetadata> files;
    int discovered_count = 0;
    int ignored_count = 0;
};

struct ScanOptions {
    std::vector<std::filesystem::path> include_paths;
    std::vector<std::filesystem::path> exclude_paths;
};

RepositoryScanResult scan_repository(const std::filesystem::path& repo_root, const ScanOptions& options = {});
std::vector<FileMetadata> scan_repository_files(const std::filesystem::path& repo_root);

} // namespace repolens
