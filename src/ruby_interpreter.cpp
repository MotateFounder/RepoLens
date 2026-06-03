#include "repolens/interpreters/ruby_interpreter.hpp"

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
    int line_start = 0;
};

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read Ruby file: " + path);
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

std::string strip_comment(const std::string& line)
{
    bool single = false;
    bool dbl = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\'' && !dbl) {
            single = !single;
        } else if (line[i] == '"' && !single) {
            dbl = !dbl;
        } else if (line[i] == '#' && !single && !dbl) {
            return line.substr(0, i);
        }
    }
    return line;
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
        parameter.name = trim(equals == std::string::npos ? part : part.substr(0, equals));
        if (equals != std::string::npos) {
            parameter.default_value = trim(part.substr(equals + 1));
        }
        parameters.push_back(parameter);
    }
    return parameters;
}

void close_scope(std::vector<Scope>& scopes, std::vector<repolens::CodeSymbol>& symbols, const std::vector<LineInfo>& lines, int line_number)
{
    if (scopes.empty()) {
        return;
    }
    auto scope = scopes.back();
    scopes.pop_back();
    auto& symbol = symbols[static_cast<std::size_t>(scope.symbol_index)];
    symbol.line_end = std::max(symbol.line_start, line_number);
    symbol.char_end = line_end_char(lines, symbol.line_end);
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
}

std::string current_parent_name(const std::vector<Scope>& scopes)
{
    return scopes.empty() ? "" : scopes.back().qualified_name;
}

int current_parent_index(const std::vector<Scope>& scopes)
{
    return scopes.empty() ? -1 : scopes.back().symbol_index;
}

void parse_ruby_source(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<Scope> scopes;
    const std::regex module_regex{R"(^\s*module\s+([A-Z][A-Za-z0-9_:]*))"};
    const std::regex class_regex{R"(^\s*class\s+([A-Z][A-Za-z0-9_:]*)(?:\s*<\s*([A-Za-z0-9_:]+))?)"};
    const std::regex method_regex{R"(^\s*def\s+(?:self\.)?([A-Za-z_][A-Za-z0-9_!?=]*)(?:\(([^)]*)\)|\s+([^#]+))?)"};
    const std::regex const_regex{R"(^\s*([A-Z][A-Za-z0-9_]*)\s*=)"};
    const std::regex attr_regex{R"(^\s*attr_(?:reader|writer|accessor)\s+(.+))"};
    const std::regex route_regex{R"(^\s*(get|post|put|patch|delete|resources|resource)\s+[:'\"]?([^,'\"\s)]+))"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto code = trim(strip_comment(lines[index].text));
        if (code.empty()) {
            continue;
        }
        std::smatch match;
        if (std::regex_search(code, match, module_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("ruby_module", name, join_qualified(current_parent_name(scopes), name), code, lines[index], line_number, current_parent_index(scopes));
            result.symbols.push_back(symbol);
            scopes.push_back({"ruby_module", name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (std::regex_search(code, match, class_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("ruby_class", name, join_qualified(current_parent_name(scopes), name), code, lines[index], line_number, current_parent_index(scopes));
            if (match.size() > 2) {
                symbol.return_type = trim(match[2].str());
            }
            result.symbols.push_back(symbol);
            scopes.push_back({"ruby_class", name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (std::regex_search(code, match, method_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol(current_parent_name(scopes).empty() ? "ruby_function" : "ruby_method", name, join_qualified(current_parent_name(scopes), name), code, lines[index], line_number, current_parent_index(scopes));
            const auto params = match[2].matched ? match[2].str() : match[3].str();
            symbol.parameters = parse_parameters(params);
            result.symbols.push_back(symbol);
            scopes.push_back({symbol.kind, name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (code == "end") {
            close_scope(scopes, result.symbols, lines, line_number);
        } else if (std::regex_search(code, match, const_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("ruby_constant", name, join_qualified(current_parent_name(scopes), name), code, lines[index], line_number, current_parent_index(scopes)));
        } else if (std::regex_search(code, match, attr_regex)) {
            std::stringstream attrs{match[1].str()};
            std::string attr;
            while (std::getline(attrs, attr, ',')) {
                attr = trim(attr);
                while (!attr.empty() && (attr.front() == ':' || attr.front() == '\'' || attr.front() == '"')) {
                    attr.erase(attr.begin());
                }
                while (!attr.empty() && (attr.back() == '\'' || attr.back() == '"')) {
                    attr.pop_back();
                }
                if (!attr.empty()) {
                    result.symbols.push_back(make_symbol("ruby_attribute", attr, join_qualified(current_parent_name(scopes), attr), code, lines[index], line_number, current_parent_index(scopes)));
                }
            }
        } else if (std::regex_search(code, match, route_regex)) {
            const auto name = match[2].str();
            auto symbol = make_symbol("rails_route", name, name, code, lines[index], line_number);
            symbol.return_type = match[1].str();
            result.symbols.push_back(symbol);
        }
    }

    while (!scopes.empty()) {
        close_scope(scopes, result.symbols, lines, static_cast<int>(lines.size()));
    }
}

void parse_gems(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex gem_regex{R"((?:\bgem|\.add_(?:runtime_)?dependency|\.add_development_dependency)\s+['"]([^'"]+)['"])"};
    const std::regex gemspec_name_regex{R"(\.name\s*=\s*['"]([^'"]+)['"])"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto code = trim(strip_comment(lines[index].text));
        const int line_number = static_cast<int>(index + 1);
        std::smatch match;
        if (std::regex_search(code, match, gem_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("ruby_gem", name, name, code, lines[index], line_number));
        } else if (std::regex_search(code, match, gemspec_name_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("ruby_gemspec", name, name, code, lines[index], line_number));
        }
    }
}

} // namespace

namespace repolens {

std::string RubyInterpreter::language_id() const
{
    return "ruby";
}

std::vector<std::string> RubyInterpreter::file_extensions() const
{
    return {".rb", ".gemspec", "Gemfile"};
}

ParseResult RubyInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto filename = filename_for(file);
    const auto extension = extension_for(file);

    if (filename == "gemfile" || extension == ".gemspec") {
        result.language = filename == "gemfile" ? "gemfile" : "gemspec";
        parse_gems(lines, result);
    } else {
        result.language = "ruby";
        parse_ruby_source(lines, result);
    }
    return result;
}

} // namespace repolens
