#include "repolens/interpreters/build_file_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace {

struct LineInfo {
    std::string text;
    int char_start = 0;
};

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read build file: " + path);
    }

    std::vector<LineInfo> lines;
    std::string line;
    int offset = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back({line, offset});
        offset += static_cast<int>(line.size()) + 1;
    }
    return lines;
}

std::string trim(const std::string& text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : "";
}

std::string lower_text(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

repolens::CodeSymbol make_symbol(
    const std::string& kind,
    const std::string& name,
    const std::string& qualified_name,
    const std::string& signature,
    const LineInfo& line,
    int line_number)
{
    repolens::CodeSymbol symbol;
    symbol.kind = kind;
    symbol.name = name;
    symbol.qualified_name = qualified_name;
    symbol.signature = trim(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    symbol.char_start = line.char_start;
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    return symbol;
}

std::string first_argument(const std::string& text, const std::string& function_name)
{
    const auto open = text.find(function_name + "(");
    if (open == std::string::npos) {
        return "";
    }
    const auto start = open + function_name.size() + 1;
    const auto close = text.find(')', start);
    auto body = close == std::string::npos ? text.substr(start) : text.substr(start, close - start);
    body = trim(body);
    std::stringstream stream{body};
    std::string value;
    stream >> value;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

} // namespace

namespace repolens {

std::string BuildFileInterpreter::language_id() const
{
    return "build";
}

std::vector<std::string> BuildFileInterpreter::file_extensions() const
{
    return {".cmake", ".sln", ".mk", ".make", "CMakeLists.txt", "Makefile"};
}

ParseResult BuildFileInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto filename = std::filesystem::path{file.relative_path}.filename().string();
    const auto lower_filename = lower_text(filename);
    const auto lines = read_lines(file.absolute_path);
    result.language = lower_filename == "makefile" || lower_filename.ends_with(".mk") || lower_filename.ends_with(".make")
        ? "make"
        : (lower_filename.ends_with(".sln") ? "solution" : "cmake");

    const std::regex make_target_regex{R"(^\s*([A-Za-z0-9_\-\.]+)\s*:\s*([^=].*)?$)"};
    const std::regex sln_project_regex{R"repolens(^Project\([^)]*\)\s*=\s*"([^"]+)",\s*"([^"]+)")repolens"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];
        const auto trimmed = trim(line.text);
        const auto lower = lower_text(trimmed);
        const int line_number = static_cast<int>(index + 1);

        if (result.language == "cmake") {
            if (lower.rfind("project(", 0) == 0) {
                const auto name = first_argument(trimmed, "project");
                if (!name.empty()) {
                    result.symbols.push_back(make_symbol("cmake_project", name, name, trimmed, line, line_number));
                }
            } else if (lower.rfind("add_executable(", 0) == 0) {
                const auto name = first_argument(trimmed, "add_executable");
                if (!name.empty()) {
                    result.symbols.push_back(make_symbol("cmake_target", name, name, trimmed, line, line_number));
                }
            } else if (lower.rfind("add_library(", 0) == 0) {
                const auto name = first_argument(trimmed, "add_library");
                if (!name.empty()) {
                    result.symbols.push_back(make_symbol("cmake_target", name, name, trimmed, line, line_number));
                }
            }
        } else if (result.language == "make") {
            std::smatch match;
            if (std::regex_search(trimmed, match, make_target_regex)) {
                const auto name = match[1].str();
                if (!name.empty() && name.find('.') != 0) {
                    result.symbols.push_back(make_symbol("make_target", name, name, trimmed, line, line_number));
                }
            }
        } else if (result.language == "solution") {
            std::smatch match;
            if (std::regex_search(trimmed, match, sln_project_regex)) {
                const auto name = match[1].str();
                const auto path = match[2].str();
                auto symbol = make_symbol("solution_project", name, name, trimmed, line, line_number);
                symbol.return_type = path;
                result.symbols.push_back(symbol);
            }
        }
    }

    return result;
}

} // namespace repolens
