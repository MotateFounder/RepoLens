#include "repolens/interpreters/go_interpreter.hpp"

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
        throw std::runtime_error("Failed to read Go file: " + path);
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

std::string extension_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.extension().string());
}

std::string filename_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.filename().string());
}

std::string strip_line_comment(const std::string& line)
{
    bool in_string = false;
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        if (line[index] == '"' && (index == 0 || line[index - 1] != '\\')) {
            in_string = !in_string;
        }
        if (!in_string && line[index] == '/' && line[index + 1] == '/') {
            return line.substr(0, index);
        }
    }
    return line;
}

int count_char(const std::string& text, char value)
{
    return static_cast<int>(std::count(text.begin(), text.end(), value));
}

std::string normalize_spaces(std::string text)
{
    std::string result;
    bool previous_space = false;
    for (char c : text) {
        const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (is_space && previous_space) {
            continue;
        }
        result.push_back(is_space ? ' ' : c);
        previous_space = is_space;
    }
    return trim(result);
}

int line_end_char(const std::vector<LineInfo>& lines, int line_number)
{
    if (line_number <= 0 || static_cast<std::size_t>(line_number) > lines.size()) {
        return 0;
    }
    const auto& line = lines[static_cast<std::size_t>(line_number - 1)];
    return line.char_start + static_cast<int>(line.text.size());
}

int find_block_end_line(const std::vector<LineInfo>& lines, std::size_t start_index)
{
    int depth = 0;
    bool saw_open = false;
    for (std::size_t index = start_index; index < lines.size(); ++index) {
        const auto line = strip_line_comment(lines[index].text);
        depth += count_char(line, '{');
        saw_open = saw_open || line.find('{') != std::string::npos;
        depth -= count_char(line, '}');
        if (saw_open && depth <= 0) {
            return static_cast<int>(index + 1);
        }
    }
    return static_cast<int>(start_index + 1);
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
    symbol.qualified_name = qualified_name.empty() ? name : qualified_name;
    symbol.signature = normalize_spaces(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto position = line.text.find(name);
    symbol.char_start = line.char_start + static_cast<int>(position == std::string::npos ? 0 : position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    return symbol;
}

std::vector<repolens::CodeParameter> parse_parameters(const std::string& text)
{
    std::vector<repolens::CodeParameter> parameters;
    std::stringstream stream{text};
    std::string part;
    int position = 0;
    while (std::getline(stream, part, ',')) {
        part = trim(part);
        if (part.empty()) {
            continue;
        }
        repolens::CodeParameter parameter;
        parameter.position = position++;
        std::stringstream item{part};
        std::vector<std::string> words;
        std::string word;
        while (item >> word) {
            words.push_back(word);
        }
        if (words.size() >= 2) {
            parameter.name = words.front();
            parameter.type = words.back();
        } else if (!words.empty()) {
            parameter.type = words.front();
        }
        parameters.push_back(parameter);
    }
    return parameters;
}

std::string join_qualified(const std::string& package_name, const std::string& name)
{
    return package_name.empty() ? name : package_name + "." + name;
}

void parse_go_source(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string package_name;
    bool in_import_block = false;
    const std::regex package_regex{R"(^\s*package\s+([A-Za-z_][A-Za-z0-9_]*))"};
    const std::regex import_regex{R"(^\s*(?:import\s+)?(?:[A-Za-z_][A-Za-z0-9_]*\s+)?\"([^\"]+)\")"};
    const std::regex function_regex{R"(^\s*func\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*([A-Za-z_][A-Za-z0-9_.*\[\]]*)?)"};
    const std::regex method_regex{R"(^\s*func\s+\(([^)]*)\)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*([A-Za-z_][A-Za-z0-9_.*\[\]]*)?)"};
    const std::regex type_regex{R"(^\s*type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(struct|interface)\b)"};
    const std::regex alias_regex{R"(^\s*type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+))"};
    const std::regex var_regex{R"(^\s*(?:var|const)\s+([A-Za-z_][A-Za-z0-9_]*)\b)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }

        std::smatch match;
        if (std::regex_search(trimmed, match, package_regex)) {
            package_name = match[1].str();
            result.symbols.push_back(make_symbol("go_package", package_name, package_name, trimmed, lines[index], line_number));
        } else if (trimmed == "import (" || trimmed == "import(") {
            in_import_block = true;
        } else if (in_import_block && trimmed == ")") {
            in_import_block = false;
        } else if ((in_import_block || trimmed.rfind("import", 0) == 0) && std::regex_search(trimmed, match, import_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("go_import", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, method_regex)) {
            const auto name = match[2].str();
            auto symbol = make_symbol("go_method", name, join_qualified(package_name, name), trimmed, lines[index], line_number);
            symbol.parameters = parse_parameters(match[3].str());
            if (match.size() > 4) {
                symbol.return_type = trim(match[4].str());
            }
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, function_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("go_function", name, join_qualified(package_name, name), trimmed, lines[index], line_number);
            symbol.parameters = parse_parameters(match[2].str());
            if (match.size() > 3) {
                symbol.return_type = trim(match[3].str());
            }
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto name = match[1].str();
            const auto kind = match[2].str() == "struct" ? "go_struct" : "go_interface";
            auto symbol = make_symbol(kind, name, join_qualified(package_name, name), trimmed, lines[index], line_number);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, alias_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("go_type", name, join_qualified(package_name, name), trimmed, lines[index], line_number);
            symbol.return_type = trim(match[2].str());
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, var_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("go_variable", name, join_qualified(package_name, name), trimmed, lines[index], line_number));
        }
    }
}

void parse_go_mod(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    bool in_require_block = false;
    const std::regex module_regex{R"(^\s*module\s+(\S+))"};
    const std::regex require_regex{R"(^\s*(?:require\s+)?([A-Za-z0-9_.\-/]+)\s+v?[A-Za-z0-9_.+\-]+)"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(strip_line_comment(lines[index].text));
        if (trimmed.empty()) {
            continue;
        }
        std::smatch match;
        if (std::regex_search(trimmed, match, module_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("go_module", name, name, trimmed, lines[index], line_number));
        } else if (trimmed == "require (" || trimmed == "require(") {
            in_require_block = true;
        } else if (in_require_block && trimmed == ")") {
            in_require_block = false;
        } else if ((in_require_block || trimmed.rfind("require", 0) == 0) && std::regex_search(trimmed, match, require_regex)) {
            const auto name = match[1].str();
            if (name != "require") {
                result.symbols.push_back(make_symbol("go_dependency", name, name, trimmed, lines[index], line_number));
            }
        }
    }
}

void parse_go_sum(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex sum_regex{R"(^\s*([A-Za-z0-9_.\-/]+)\s+v?[A-Za-z0-9_.+\-/]+)"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(lines[index].text);
        std::smatch match;
        if (std::regex_search(trimmed, match, sum_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("go_dependency", name, name, trimmed, lines[index], line_number));
        }
    }
}

} // namespace

namespace repolens {

std::string GoInterpreter::language_id() const
{
    return "go";
}

std::vector<std::string> GoInterpreter::file_extensions() const
{
    return {".go", "go.mod", "go.sum"};
}

ParseResult GoInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto extension = extension_for(file);
    const auto filename = filename_for(file);

    if (filename == "go.mod") {
        result.language = "go-module";
        parse_go_mod(lines, result);
    } else if (filename == "go.sum") {
        result.language = "go-sum";
        parse_go_sum(lines, result);
    } else {
        result.language = "go";
        parse_go_source(lines, result);
    }

    (void)extension;
    return result;
}

} // namespace repolens
