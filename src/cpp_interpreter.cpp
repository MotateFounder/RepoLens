#include "repolens/interpreters/cpp_interpreter.hpp"

#include <algorithm>
#include <cctype>
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
        throw std::runtime_error("Failed to read C/C++ file: " + path);
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

std::string strip_line_comment(const std::string& text)
{
    const auto position = text.find("//");
    return position == std::string::npos ? text : text.substr(0, position);
}

int count_char(const std::string& text, char value)
{
    return static_cast<int>(std::count(text.begin(), text.end(), value));
}

std::string join_qualified(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "::" + name;
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

bool is_identifier_like(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text.front());
    if (!(std::isalpha(first) || text.front() == '_' || text.front() == '~')) {
        return false;
    }
    for (char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (!(std::isalnum(value) || character == '_' || character == ':' || character == '~')) {
            return false;
        }
    }
    return true;
}

bool starts_with_statement_keyword(const std::string& text)
{
    std::stringstream stream{text};
    std::string word;
    stream >> word;
    return word == "if" || word == "for" || word == "while" || word == "switch" || word == "return" ||
        word == "catch" || word == "else" || word == "do" || word == "delete" || word == "new";
}

std::string current_parent(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "namespace" || scope->kind == "class" || scope->kind == "struct") {
            return scope->qualified_name;
        }
    }
    return "";
}

const Scope* current_type(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "class" || scope->kind == "struct") {
            return &*scope;
        }
    }
    return nullptr;
}

std::vector<repolens::CodeParameter> parse_parameters(const std::string& text)
{
    std::vector<repolens::CodeParameter> parameters;
    std::stringstream stream{text};
    std::string part;
    int position = 0;
    while (std::getline(stream, part, ',')) {
        part = trim(part);
        if (part.empty() || part == "void") {
            continue;
        }
        const auto equals = part.find('=');
        const auto declaration = trim(equals == std::string::npos ? part : part.substr(0, equals));
        std::stringstream words{declaration};
        std::vector<std::string> tokens;
        std::string token;
        while (words >> token) {
            tokens.push_back(token);
        }
        if (tokens.empty()) {
            continue;
        }

        repolens::CodeParameter parameter;
        parameter.position = position++;
        parameter.name = tokens.back();
        if (tokens.size() >= 2) {
            parameter.type = tokens[tokens.size() - 2];
        }
        if (equals != std::string::npos) {
            parameter.default_value = trim(part.substr(equals + 1));
        }
        parameters.push_back(parameter);
    }
    return parameters;
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

int line_end_char(const std::vector<LineInfo>& lines, int line_number)
{
    if (line_number <= 0 || static_cast<std::size_t>(line_number) > lines.size()) {
        return 0;
    }
    const auto& line = lines[static_cast<std::size_t>(line_number - 1)];
    return line.char_start + static_cast<int>(line.text.size());
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
    symbol.qualified_name = qualified_name;
    symbol.signature = normalize_spaces(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto name_position = line.text.find(name);
    symbol.char_start = line.char_start + static_cast<int>(name_position == std::string::npos ? 0 : name_position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    symbol.parent_index = parent_index;
    return symbol;
}

std::string function_name_from_declaration(const std::string& declaration)
{
    std::stringstream stream{declaration};
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    if (words.empty()) {
        return "";
    }
    auto name = words.back();
    const auto pointer = name.find_last_of("*&");
    if (pointer != std::string::npos && pointer + 1 < name.size()) {
        name = name.substr(pointer + 1);
    }
    return name;
}

std::string return_type_from_declaration(const std::string& declaration, const std::string& name)
{
    const auto position = declaration.rfind(name);
    if (position == std::string::npos || position == 0) {
        return "";
    }
    return trim(declaration.substr(0, position));
}

} // namespace

namespace repolens {

std::string CppInterpreter::language_id() const
{
    return "cpp";
}

std::vector<std::string> CppInterpreter::file_extensions() const
{
    return {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl"};
}

ParseResult CppInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    result.language = language_id();
    const auto lines = read_lines(file.absolute_path);
    std::vector<Scope> scopes;
    int brace_depth = 0;

    const std::regex namespace_regex{R"(^\s*namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{?)"};
    const std::regex type_regex{R"(^\s*(class|struct|enum)\s+(class\s+)?([A-Za-z_][A-Za-z0-9_]*)[^;]*\{?)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        std::smatch match;

        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.rfind("#define ", 0) == 0) {
            std::stringstream stream{trimmed.substr(8)};
            std::string name;
            stream >> name;
            const auto paren = name.find('(');
            if (paren != std::string::npos) {
                name = name.substr(0, paren);
            }
            if (!name.empty()) {
                result.symbols.push_back(make_symbol("macro", name, join_qualified(current_parent(scopes), name), trimmed, lines[index], line_number));
            }
        } else if (std::regex_search(trimmed, match, namespace_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("namespace", name, join_qualified(current_parent(scopes), name), trimmed, lines[index], line_number);
            result.symbols.push_back(symbol);
            scopes.push_back({"namespace", name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto kind = match[1].str();
            const auto name = match[3].str();
            const auto parent = current_parent(scopes);
            auto symbol = make_symbol(kind, name, join_qualified(parent, name), trimmed, lines[index], line_number);
            symbol.parent_index = current_type(scopes) ? current_type(scopes)->symbol_index : -1;
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (!starts_with_statement_keyword(trimmed) && trimmed.find('(') != std::string::npos &&
            trimmed.find(')') != std::string::npos && trimmed.find(';') == std::string::npos) {
            const auto open = trimmed.find('(');
            const auto close = trimmed.find(')', open);
            const auto declaration = trim(trimmed.substr(0, open));
            auto name = function_name_from_declaration(declaration);
            const auto scope = name.find_last_of(':');
            if (scope != std::string::npos && scope + 1 < name.size()) {
                name = name.substr(scope + 1);
            }
            if (is_identifier_like(name)) {
                const auto* parent_type = current_type(scopes);
                const auto parent = current_parent(scopes);
                auto symbol = make_symbol(
                    parent_type && (name == parent_type->name || name == "~" + parent_type->name) ? "constructor" : "function",
                    name,
                    join_qualified(parent, name),
                    trimmed,
                    lines[index],
                    line_number,
                    parent_type ? parent_type->symbol_index : -1);
                symbol.return_type = return_type_from_declaration(declaration, name);
                symbol.parameters = parse_parameters(trimmed.substr(open + 1, close - open - 1));
                symbol.line_end = find_block_end_line(lines, index);
                symbol.char_end = line_end_char(lines, symbol.line_end);
                symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
                result.symbols.push_back(symbol);
            }
        } else if (const auto* parent_type = current_type(scopes);
            parent_type && brace_depth == parent_type->close_depth && trimmed.find(';') != std::string::npos &&
            trimmed.find('(') == std::string::npos) {
            const auto declaration = trim(trimmed.substr(0, trimmed.find(';')));
            std::stringstream stream{declaration};
            std::vector<std::string> words;
            std::string word;
            while (stream >> word) {
                words.push_back(word);
            }
            if (words.size() >= 2) {
                auto name = words.back();
                const auto equals = name.find('=');
                if (equals != std::string::npos) {
                    name = name.substr(0, equals);
                }
                if (is_identifier_like(name)) {
                    auto symbol = make_symbol("field", name, join_qualified(parent_type->qualified_name, name), trimmed, lines[index], line_number, parent_type->symbol_index);
                    symbol.return_type = words[words.size() - 2];
                    result.symbols.push_back(symbol);
                }
            }
        }

        brace_depth += count_char(line, '{');
        brace_depth -= count_char(line, '}');

        while (!scopes.empty() && scopes.back().close_depth > 0 && line_number > scopes.back().line_start &&
            brace_depth < scopes.back().close_depth) {
            auto& symbol = result.symbols[scopes.back().symbol_index];
            symbol.line_end = line_number;
            symbol.char_end = lines[index].char_start + static_cast<int>(lines[index].text.size());
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            scopes.pop_back();
        }
    }

    for (auto& symbol : result.symbols) {
        if (symbol.line_end <= 0) {
            symbol.line_end = symbol.line_start;
        }
    }

    return result;
}

} // namespace repolens
