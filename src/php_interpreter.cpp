#include "repolens/interpreters/php_interpreter.hpp"

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

struct Scope {
    std::string kind;
    std::string name;
    std::string qualified_name;
    int symbol_index = -1;
    int close_depth = 0;
    int line_start = 0;
};

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read PHP file: " + path);
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

std::string filename_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.filename().string());
}

std::string extension_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.extension().string());
}

std::string strip_line_comment(const std::string& line)
{
    bool in_string = false;
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        if (line[index] == '"' && (index == 0 || line[index - 1] != '\\')) {
            in_string = !in_string;
        }
        if (!in_string && ((line[index] == '/' && line[index + 1] == '/') || line[index] == '#')) {
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

std::string join_qualified(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "\\" + name;
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
        const auto equals = part.find('=');
        if (equals != std::string::npos) {
            parameter.default_value = trim(part.substr(equals + 1));
            part = trim(part.substr(0, equals));
        }
        const auto dollar = part.find('$');
        if (dollar != std::string::npos) {
            parameter.name = part.substr(dollar);
            parameter.type = trim(part.substr(0, dollar));
        } else {
            parameter.name = part;
        }
        parameters.push_back(parameter);
    }
    return parameters;
}

repolens::CodeSymbol make_symbol(
    const std::string& kind,
    const std::string& name,
    const std::string& qualified_name,
    const std::string& signature,
    const LineInfo& line,
    int line_number,
    int parent_index = -1)
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
    symbol.parent_index = parent_index;
    return symbol;
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

const Scope* current_type(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "php_class" || scope->kind == "php_interface" || scope->kind == "php_trait" || scope->kind == "php_enum") {
            return &*scope;
        }
    }
    return nullptr;
}

void parse_php_source(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string namespace_name;
    std::vector<Scope> scopes;
    int brace_depth = 0;
    static const std::regex namespace_regex{R"(^\s*namespace\s+([A-Za-z_\\][A-Za-z0-9_\\]*))"};
    static const std::regex use_regex{R"(^\s*use\s+([^;]+);)"};
    static const std::regex type_regex{R"(^\s*(?:(?:abstract|final|readonly)\s+)*(class|interface|trait|enum)\s+([A-Za-z_][A-Za-z0-9_]*)(.*))"};
    static const std::regex function_regex{R"(^\s*(?:(?:public|private|protected|static|final|abstract)\s+)*function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?::\s*([A-Za-z_\\?][A-Za-z0-9_\\|?]*))?)"};
    static const std::regex property_regex{R"(^\s*(?:(?:public|private|protected|static|readonly)\s+)+(?:(?:[A-Za-z_\\?][A-Za-z0-9_\\|?]*|\?[\w\\]+)\s+)?(\$[A-Za-z_][A-Za-z0-9_]*)\s*(?:=.*)?;)"}; 
    static const std::regex const_regex{R"(^\s*(?:(?:public|private|protected)\s+)?const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed == "<?php" || trimmed.rfind("/*", 0) == 0 || trimmed.rfind("*", 0) == 0) {
            brace_depth += count_char(line, '{') - count_char(line, '}');
            continue;
        }

        while (!scopes.empty() && trimmed != "{" && line_number > scopes.back().line_start && brace_depth < scopes.back().close_depth) {
            auto& symbol = result.symbols[static_cast<std::size_t>(scopes.back().symbol_index)];
            symbol.line_end = std::max(symbol.line_start, line_number - 1);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            scopes.pop_back();
        }

        std::smatch match;
        if (std::regex_search(trimmed, match, namespace_regex)) {
            namespace_name = match[1].str();
            result.symbols.push_back(make_symbol("php_namespace", namespace_name, namespace_name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, use_regex)) {
            const auto name = trim(match[1].str());
            result.symbols.push_back(make_symbol("php_use", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto kind = std::string{"php_"} + match[1].str();
            const auto name = match[2].str();
            const auto qualified = join_qualified(namespace_name, name);
            auto symbol = make_symbol(kind, name, qualified, trimmed, lines[index], line_number);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, qualified, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (std::regex_search(trimmed, match, function_regex)) {
            const auto name = match[1].str();
            const auto* parent = current_type(scopes);
            const auto kind = parent ? (name == "__construct" ? "php_constructor" : "php_method") : "php_function";
            auto symbol = make_symbol(kind, name, join_qualified(parent ? parent->qualified_name : namespace_name, name), trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.parameters = parse_parameters(match[2].str());
            if (match.size() > 3) {
                symbol.return_type = trim(match[3].str());
            }
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (const auto* parent = current_type(scopes); parent && std::regex_search(trimmed, match, property_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("php_property", name, join_qualified(parent->qualified_name, name), trimmed, lines[index], line_number, parent->symbol_index));
        } else if (const auto* parent = current_type(scopes); parent && std::regex_search(trimmed, match, const_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("php_constant", name, join_qualified(parent->qualified_name, name), trimmed, lines[index], line_number, parent->symbol_index));
        }

        brace_depth += count_char(line, '{') - count_char(line, '}');
    }
}

std::string quoted_value_after_colon(const std::string& line)
{
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return "";
    }
    const auto quote = line.find('"', colon + 1);
    if (quote == std::string::npos) {
        return "";
    }
    const auto close = line.find('"', quote + 1);
    if (close == std::string::npos) {
        return "";
    }
    return line.substr(quote + 1, close - quote - 1);
}

void parse_composer_json(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.find("\"require\"") != std::string::npos) {
            section = "require";
        } else if (trimmed.find("\"require-dev\"") != std::string::npos) {
            section = "require-dev";
        } else if (trimmed.find("\"autoload\"") != std::string::npos) {
            section = "autoload";
        }

        if (trimmed.rfind("\"name\"", 0) == 0) {
            const auto name = quoted_value_after_colon(trimmed);
            if (!name.empty()) {
                result.symbols.push_back(make_symbol("composer_package", name, name, trimmed, lines[index], line_number));
            }
        } else if ((section == "require" || section == "require-dev") && trimmed.size() > 2 && trimmed.front() == '"') {
            const auto close = trimmed.find('"', 1);
            if (close != std::string::npos) {
                const auto name = trimmed.substr(1, close - 1);
                if (!name.empty() && name != "require" && name != "require-dev") {
                    result.symbols.push_back(make_symbol("composer_dependency", name, name, trimmed, lines[index], line_number));
                }
            }
        } else if (section == "autoload" && trimmed.find("\"psr-4\"") != std::string::npos) {
            result.symbols.push_back(make_symbol("composer_autoload", "psr-4", "autoload.psr-4", trimmed, lines[index], line_number));
        }

        if (trimmed.find('}') != std::string::npos && section != "autoload") {
            section.clear();
        }
    }
}

} // namespace

namespace repolens {

std::string PhpInterpreter::language_id() const
{
    return "php";
}

std::vector<std::string> PhpInterpreter::file_extensions() const
{
    return {".php", "composer.json"};
}

ParseResult PhpInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto filename = filename_for(file);
    const auto extension = extension_for(file);

    if (filename == "composer.json") {
        result.language = "composer";
        parse_composer_json(lines, result);
    } else {
        result.language = "php";
        parse_php_source(lines, result);
    }

    (void)extension;
    return result;
}

} // namespace repolens
