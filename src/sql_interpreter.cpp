#include "repolens/interpreters/sql_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace {

struct LineInfo { std::string text; int char_start = 0; };

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("Failed to read SQL file: " + path);
    std::vector<LineInfo> lines;
    std::string line;
    int offset = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string filename_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.filename().string());
}

std::string strip_comment(const std::string& line)
{
    const auto position = line.find("--");
    return position == std::string::npos ? line : line.substr(0, position);
}

std::string clean_sql_name(std::string name)
{
    name = trim(name);
    while (!name.empty() && (name.front() == '"' || name.front() == '[' || name.front() == '`')) name.erase(name.begin());
    while (!name.empty() && (name.back() == '"' || name.back() == ']' || name.back() == '`' || name.back() == ';' || name.back() == '(')) name.pop_back();
    return name;
}

repolens::CodeSymbol make_symbol(const std::string& kind, const std::string& name, const std::string& signature, const LineInfo& line, int line_number)
{
    repolens::CodeSymbol symbol;
    symbol.kind = kind;
    symbol.name = clean_sql_name(name);
    symbol.qualified_name = symbol.name;
    symbol.signature = trim(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto position = line.text.find(name);
    symbol.char_start = line.char_start + static_cast<int>(position == std::string::npos ? 0 : position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    return symbol;
}

bool looks_like_migration(const repolens::FileMetadata& file)
{
    const auto text = lower_text(file.relative_path);
    return text.find("migration") != std::string::npos || text.find("migrations") != std::string::npos;
}

} // namespace

namespace repolens {

std::string SqlInterpreter::language_id() const { return "sql"; }

std::vector<std::string> SqlInterpreter::file_extensions() const { return {".sql"}; }

ParseResult SqlInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    result.language = "sql";
    const auto lines = read_lines(file.absolute_path);
    if (looks_like_migration(file)) {
        result.symbols.push_back(make_symbol("sql_migration", filename_for(file), file.relative_path, lines.empty() ? LineInfo{} : lines.front(), 1));
    }

    static const std::regex object_regex{R"(^\s*(?:CREATE\s+(?:OR\s+REPLACE\s+)?|ALTER\s+|DROP\s+)?(TABLE|VIEW|INDEX|PROCEDURE|PROC|FUNCTION|TRIGGER|SCHEMA)\s+(?:IF\s+(?:NOT\s+)?EXISTS\s+)?([A-Za-z0-9_\.\[\]`"]+))", std::regex_constants::icase};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto code = trim(strip_comment(lines[index].text));
        std::smatch match;
        if (std::regex_search(code, match, object_regex)) {
            auto object_type = lower_text(match[1].str());
            if (object_type == "proc") object_type = "procedure";
            result.symbols.push_back(make_symbol("sql_" + object_type, match[2].str(), code, lines[index], static_cast<int>(index + 1)));
        }
    }
    return result;
}

} // namespace repolens
