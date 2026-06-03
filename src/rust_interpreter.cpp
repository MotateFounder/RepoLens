#include "repolens/interpreters/rust_interpreter.hpp"

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
        throw std::runtime_error("Failed to read Rust file: " + path);
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

std::string join_qualified(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "::" + name;
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
        const auto colon = part.find(':');
        parameter.name = trim(colon == std::string::npos ? part : part.substr(0, colon));
        if (colon != std::string::npos) {
            parameter.type = trim(part.substr(colon + 1));
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

const Scope* current_scope(const std::vector<Scope>& scopes)
{
    return scopes.empty() ? nullptr : &scopes.back();
}

void parse_rust_source(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<Scope> scopes;
    int brace_depth = 0;
    const std::regex module_regex{R"(^\s*(?:pub\s+)?mod\s+([A-Za-z_][A-Za-z0-9_]*))"};
    const std::regex use_regex{R"(^\s*use\s+([^;]+);)"};
    const std::regex type_regex{R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(struct|enum|trait)\s+([A-Za-z_][A-Za-z0-9_]*))"};
    const std::regex impl_regex{R"(^\s*impl(?:\s*<[^>]+>)?(?:\s+([A-Za-z_][A-Za-z0-9_:<>]*))?(?:\s+for\s+([A-Za-z_][A-Za-z0-9_:<>]*))?)"};
    const std::regex function_regex{R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:->\s*([^{;]+))?)"};
    const std::regex const_regex{R"(^\s*(?:pub\s+)?(?:const|static)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:)"};
    const std::regex macro_regex{R"(^\s*macro_rules!\s+([A-Za-z_][A-Za-z0-9_]*)|^\s*([A-Za-z_][A-Za-z0-9_]*)!\s*\()"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.rfind("#[", 0) == 0) {
            brace_depth += count_char(line, '{') - count_char(line, '}');
            continue;
        }

        while (!scopes.empty() && line_number > scopes.back().line_start && brace_depth < scopes.back().close_depth) {
            auto& symbol = result.symbols[static_cast<std::size_t>(scopes.back().symbol_index)];
            symbol.line_end = std::max(symbol.line_start, line_number - 1);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            scopes.pop_back();
        }

        std::smatch match;
        if (std::regex_search(trimmed, match, use_regex)) {
            const auto name = trim(match[1].str());
            result.symbols.push_back(make_symbol("rust_use", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto kind = std::string{"rust_"} + match[1].str();
            const auto name = match[2].str();
            const auto* parent = current_scope(scopes);
            const auto qualified = join_qualified(parent ? parent->qualified_name : "", name);
            auto symbol = make_symbol(kind, name, qualified, trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, qualified, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (std::regex_search(trimmed, match, impl_regex) && trimmed.find('{') != std::string::npos) {
            const auto target = match[2].matched ? match[2].str() : match[1].str();
            if (!target.empty()) {
                auto symbol = make_symbol("rust_impl", target, target, trimmed, lines[index], line_number);
                symbol.line_end = find_block_end_line(lines, index);
                symbol.char_end = line_end_char(lines, symbol.line_end);
                symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
                result.symbols.push_back(symbol);
                scopes.push_back({"rust_impl", target, target, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
            }
        } else if (std::regex_search(trimmed, match, module_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("rust_module", name, name, trimmed, lines[index], line_number);
            symbol.line_end = trimmed.find('{') == std::string::npos ? line_number : find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            if (trimmed.find('{') != std::string::npos) {
                scopes.push_back({"rust_module", name, name, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
            }
        } else if (std::regex_search(trimmed, match, function_regex)) {
            const auto name = match[1].str();
            const auto* parent = current_scope(scopes);
            const auto kind = parent && parent->kind == "rust_impl" ? "rust_method" : "rust_function";
            auto symbol = make_symbol(kind, name, join_qualified(parent ? parent->qualified_name : "", name), trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.parameters = parse_parameters(match[2].str());
            if (match.size() > 3) {
                symbol.return_type = trim(match[3].str());
            }
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, const_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("rust_constant", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, macro_regex)) {
            const auto name = match[1].matched ? match[1].str() : match[2].str();
            result.symbols.push_back(make_symbol(match[1].matched ? "rust_macro" : "rust_macro_call", name, name, trimmed, lines[index], line_number));
        }

        brace_depth += count_char(line, '{') - count_char(line, '}');
    }
}

std::string strip_quotes(std::string text)
{
    text = trim(text);
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

void parse_cargo_toml(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    const std::regex section_regex{R"(^\s*\[([^\]]+)\])"};
    const std::regex property_regex{R"(^\s*([A-Za-z0-9_.\-]+)\s*=\s*(.+))"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        std::smatch match;
        if (std::regex_search(trimmed, match, section_regex)) {
            section = match[1].str();
            result.symbols.push_back(make_symbol("cargo_section", section, section, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, property_regex)) {
            const auto name = match[1].str();
            const auto value = strip_quotes(match[2].str());
            if (section == "package" && name == "name") {
                result.symbols.push_back(make_symbol("cargo_package", value, value, trimmed, lines[index], line_number));
            } else if (section.find("dependencies") != std::string::npos || section == "build-dependencies") {
                result.symbols.push_back(make_symbol("cargo_dependency", name, name, trimmed, lines[index], line_number));
            } else {
                result.symbols.push_back(make_symbol("cargo_property", name, section.empty() ? name : section + "." + name, trimmed, lines[index], line_number));
            }
        }
    }
}

void parse_cargo_lock(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    bool in_package = false;
    const std::regex name_regex{R"REGEX(^\s*name\s*=\s*"([^"]+)")REGEX"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(lines[index].text);
        if (trimmed == "[[package]]") {
            in_package = true;
            continue;
        }
        std::smatch match;
        if (in_package && std::regex_search(trimmed, match, name_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("cargo_dependency", name, name, trimmed, lines[index], line_number));
            in_package = false;
        }
    }
}

} // namespace

namespace repolens {

std::string RustInterpreter::language_id() const
{
    return "rust";
}

std::vector<std::string> RustInterpreter::file_extensions() const
{
    return {".rs", "Cargo.toml", "Cargo.lock"};
}

ParseResult RustInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto filename = filename_for(file);
    const auto extension = extension_for(file);

    if (filename == "cargo.toml") {
        result.language = "cargo";
        parse_cargo_toml(lines, result);
    } else if (filename == "cargo.lock") {
        result.language = "cargo-lock";
        parse_cargo_lock(lines, result);
    } else {
        result.language = "rust";
        parse_rust_source(lines, result);
    }

    (void)extension;
    return result;
}

} // namespace repolens
