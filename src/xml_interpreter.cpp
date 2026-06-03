#include "repolens/interpreters/xml_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
        throw std::runtime_error("Failed to read XML file: " + path);
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

std::string file_stem(const repolens::FileMetadata& file)
{
    return std::filesystem::path{file.relative_path}.stem().string();
}

std::string last_name_part(const std::string& text)
{
    const auto dot = text.find_last_of('.');
    return dot == std::string::npos ? text : text.substr(dot + 1);
}

std::string tag_name(const std::string& line)
{
    const auto open = line.find('<');
    if (open == std::string::npos || open + 1 >= line.size()) {
        return "";
    }
    if (line[open + 1] == '/' || line[open + 1] == '!' || line[open + 1] == '?') {
        return "";
    }

    auto end = open + 1;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '>' && line[end] != '/') {
        ++end;
    }
    return line.substr(open + 1, end - open - 1);
}

std::string attribute_value(const std::string& line, const std::string& name)
{
    auto position = line.find(name + "=");
    if (position == std::string::npos) {
        position = line.find(name + " =");
    }
    if (position == std::string::npos) {
        return "";
    }

    position = line.find_first_of("\"'", position + name.size());
    if (position == std::string::npos) {
        return "";
    }

    const char quote = line[position];
    const auto end = line.find(quote, position + 1);
    if (end == std::string::npos) {
        return "";
    }
    return line.substr(position + 1, end - position - 1);
}

repolens::CodeSymbol make_symbol(
    const std::string& language,
    const std::string& kind,
    const std::string& name,
    const std::string& qualified_name,
    const std::string& signature,
    const LineInfo& line,
    int line_number,
    int parent_index = -1)
{
    repolens::CodeSymbol symbol;
    (void)language;
    symbol.kind = kind;
    symbol.name = name;
    symbol.qualified_name = qualified_name;
    symbol.signature = trim(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    symbol.char_start = line.char_start + static_cast<int>(line.text.find('<'));
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    symbol.parent_index = parent_index;
    return symbol;
}

void parse_csproj(const repolens::FileMetadata& file, const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::string project_name = file_stem(file);

    repolens::CodeSymbol project;
    project.kind = "project";
    project.name = project_name;
    project.qualified_name = project_name;
    project.signature = file.relative_path;
    project.line_start = lines.empty() ? 1 : 1;
    project.line_end = static_cast<int>(std::max<std::size_t>(1, lines.size()));
    project.char_start = 0;
    project.char_end = static_cast<int>(std::max<long long>(0, file.char_count));
    project.char_count = project.char_end;
    result.symbols.push_back(project);
    const int project_index = 0;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];
        const auto trimmed = trim(line.text);
        const auto lower = lower_text(trimmed);
        const int line_number = static_cast<int>(index + 1);

        if (lower.find("<packagereference") != std::string::npos) {
            const auto include = attribute_value(trimmed, "Include");
            if (!include.empty()) {
                result.symbols.push_back(make_symbol(
                    result.language,
                    "package_reference",
                    include,
                    project_name + "." + include,
                    trimmed,
                    line,
                    line_number,
                    project_index));
            }
        } else if (lower.find("<projectreference") != std::string::npos) {
            const auto include = attribute_value(trimmed, "Include");
            if (!include.empty()) {
                result.symbols.push_back(make_symbol(
                    result.language,
                    "project_reference",
                    std::filesystem::path{include}.stem().string(),
                    project_name + "." + include,
                    trimmed,
                    line,
                    line_number,
                    project_index));
            }
        }
    }
}

void parse_xaml(const repolens::FileMetadata& file, const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    int root_index = -1;
    std::string root_qualified_name = file_stem(file);

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];
        const auto trimmed = trim(line.text);
        const int line_number = static_cast<int>(index + 1);
        const auto tag = tag_name(trimmed);
        if (tag.empty()) {
            continue;
        }

        if (root_index < 0) {
            const auto class_name = attribute_value(trimmed, "x:Class");
            root_qualified_name = class_name.empty() ? file_stem(file) : class_name;
            auto root = make_symbol(
                result.language,
                "xaml_class",
                last_name_part(root_qualified_name),
                root_qualified_name,
                trimmed,
                line,
                line_number);
            root.return_type = tag;
            root.line_end = static_cast<int>(std::max<std::size_t>(1, lines.size()));
            result.symbols.push_back(root);
            root_index = static_cast<int>(result.symbols.size() - 1);
        }

        auto element_name = attribute_value(trimmed, "x:Name");
        if (element_name.empty()) {
            element_name = attribute_value(trimmed, "Name");
        }
        if (!element_name.empty()) {
            auto symbol = make_symbol(
                result.language,
                "xaml_element",
                element_name,
                root_qualified_name + "." + element_name,
                trimmed,
                line,
                line_number,
                root_index);
            symbol.return_type = tag;
            result.symbols.push_back(symbol);
        }
    }
}

} // namespace

namespace repolens {

std::string XmlInterpreter::language_id() const
{
    return "xml";
}

std::vector<std::string> XmlInterpreter::file_extensions() const
{
    return {".csproj", ".xaml", ".vcxproj", ".filters", ".jucer"};
}

ParseResult XmlInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto extension = lower_text(std::filesystem::path{file.relative_path}.extension().string());
    result.language = extension == ".xaml" ? "xaml" : (extension == ".jucer" ? "juce" : "msbuild");

    const auto lines = read_lines(file.absolute_path);
    if (extension == ".csproj" || extension == ".vcxproj" || extension == ".filters" || extension == ".jucer") {
        parse_csproj(file, lines, result);
    } else if (extension == ".xaml") {
        parse_xaml(file, lines, result);
    }

    return result;
}

} // namespace repolens
