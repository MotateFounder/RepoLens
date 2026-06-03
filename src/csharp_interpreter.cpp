#include "repolens/interpreters/csharp_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
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

std::string trim(const std::string& text)
{
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : "";
}

std::string strip_line_comment(const std::string& line)
{
    const auto position = line.find("//");
    return position == std::string::npos ? line : line.substr(0, position);
}

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read C# file: " + path);
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

int count_char(const std::string& text, char value)
{
    return static_cast<int>(std::count(text.begin(), text.end(), value));
}

std::string join_qualified(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "." + name;
}

std::string current_namespace(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "namespace") {
            return scope->qualified_name;
        }
    }

    return "";
}

const Scope* current_type(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "class" || scope->kind == "interface" || scope->kind == "struct" || scope->kind == "enum") {
            return &*scope;
        }
    }

    return nullptr;
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

std::string modifiers_from_prefix(const std::string& prefix)
{
    std::string result;
    std::stringstream stream{prefix};
    std::string token;
    while (stream >> token) {
        if (token == "public" || token == "private" || token == "protected" || token == "internal" || token == "static" ||
            token == "async" || token == "virtual" || token == "override" || token == "abstract" || token == "sealed" ||
            token == "readonly" || token == "partial" || token == "extern" || token == "const" || token == "volatile") {
            if (!result.empty()) {
                result += " ";
            }
            result += token;
        }
    }
    return result;
}

std::string visibility_from_modifiers(const std::string& modifiers)
{
    for (const std::string visibility : {"public", "private", "protected", "internal"}) {
        if (modifiers.find(visibility) != std::string::npos) {
            return visibility;
        }
    }
    return "";
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

        const auto equals = part.find('=');
        std::string declaration = equals == std::string::npos ? part : trim(part.substr(0, equals));
        std::string default_value = equals == std::string::npos ? "" : trim(part.substr(equals + 1));

        std::stringstream declaration_stream{declaration};
        std::vector<std::string> tokens;
        std::string token;
        while (declaration_stream >> token) {
            tokens.push_back(token);
        }

        if (tokens.empty()) {
            continue;
        }

        repolens::CodeParameter parameter;
        parameter.position = position++;
        parameter.name = tokens.back();
        parameter.default_value = default_value;

        if (tokens.size() >= 2 && (tokens.front() == "ref" || tokens.front() == "out" || tokens.front() == "in")) {
            parameter.direction = tokens.front();
            parameter.type = tokens[tokens.size() - 2];
        } else if (tokens.size() >= 2) {
            parameter.type = tokens[tokens.size() - 2];
        }

        parameters.push_back(parameter);
    }

    return parameters;
}

std::vector<std::string> split_words(const std::string& text)
{
    std::stringstream stream{text};
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

std::vector<std::string> split_base_types(const std::string& text)
{
    std::vector<std::string> bases;
    std::stringstream stream{text};
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        const auto generic_start = item.find('<');
        if (generic_start != std::string::npos) {
            item = item.substr(0, generic_start);
        }
        if (!item.empty()) {
            bases.push_back(item);
        }
    }
    return bases;
}

bool is_modifier_word(const std::string& word)
{
    return word == "public" || word == "private" || word == "protected" || word == "internal" || word == "static" ||
        word == "async" || word == "virtual" || word == "override" || word == "abstract" || word == "sealed" ||
        word == "readonly" || word == "partial" || word == "extern" || word == "const" || word == "volatile";
}

std::vector<std::string> without_modifiers(const std::vector<std::string>& words)
{
    std::vector<std::string> result;
    for (const auto& word : words) {
        if (!is_modifier_word(word)) {
            result.push_back(word);
        }
    }
    return result;
}

bool starts_with_statement_keyword(const std::string& text)
{
    const auto words = split_words(text);
    if (words.empty()) {
        return false;
    }

    return words.front() == "return" || words.front() == "throw" || words.front() == "break" || words.front() == "continue" ||
        words.front() == "if" || words.front() == "for" || words.front() == "foreach" || words.front() == "while" ||
        words.front() == "switch" || words.front() == "using";
}

bool contains_statement_operator(const std::string& text)
{
    return text.find("+=") != std::string::npos || text.find("-=") != std::string::npos ||
        text.find("=>") != std::string::npos || text.find("==") != std::string::npos ||
        text.find("!=") != std::string::npos || text.find("<=") != std::string::npos ||
        text.find(">=") != std::string::npos;
}

bool is_identifier_like(const std::string& text)
{
    if (text.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text.front());
    if (!(std::isalpha(first) || text.front() == '_' || text.front() == '@')) {
        return false;
    }
    for (char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (!(std::isalnum(value) || character == '_' || character == '@' || character == '<' || character == '>' ||
                character == '[' || character == ']' || character == '?' || character == '.')) {
            return false;
        }
    }
    return true;
}

std::string prefix_before_name(const std::string& line, const std::string& name)
{
    const auto position = line.find(name);
    return position == std::string::npos ? "" : line.substr(0, position);
}

std::string declaration_before_any(const std::string& line, const std::string& markers)
{
    const auto position = line.find_first_of(markers);
    return position == std::string::npos ? line : line.substr(0, position);
}

void finish_symbol_line_range(repolens::CodeSymbol& symbol, int line_count)
{
    if (symbol.line_end <= 0) {
        symbol.line_end = symbol.line_start;
    }
    if (symbol.char_end <= 0) {
        symbol.char_end = symbol.char_start + symbol.char_count;
    }
    if (symbol.char_count <= 0) {
        symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    }
    symbol.line_end = std::min(symbol.line_end, line_count);
}

int find_block_end_line(const std::vector<LineInfo>& lines, std::size_t start_index)
{
    int depth = 0;
    bool saw_open = false;

    for (std::size_t index = start_index; index < lines.size(); ++index) {
        const auto line = strip_line_comment(lines[index].text);
        depth += count_char(line, '{');
        if (line.find('{') != std::string::npos) {
            saw_open = true;
        }
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

} // namespace

namespace repolens {

std::string CSharpInterpreter::language_id() const
{
    return "csharp";
}

std::vector<std::string> CSharpInterpreter::file_extensions() const
{
    return {".cs"};
}

ParseResult CSharpInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    result.language = language_id();

    const auto lines = read_lines(file.absolute_path);
    std::vector<Scope> scopes;
    int brace_depth = 0;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const std::string line = strip_line_comment(lines[index].text);
        const std::string trimmed = trim(line);

        if (trimmed.rfind("namespace ", 0) == 0) {
            auto namespace_name = trim(trimmed.substr(std::string{"namespace "}.size()));
            const auto marker = namespace_name.find_first_of(" {;");
            if (marker != std::string::npos) {
                namespace_name = namespace_name.substr(0, marker);
            }

            CodeSymbol symbol;
            symbol.kind = "namespace";
            symbol.name = namespace_name;
            symbol.qualified_name = symbol.name;
            symbol.signature = normalize_spaces(trimmed);
            symbol.line_start = line_number;
            symbol.line_end = line_number;
            symbol.char_start = lines[index].char_start + static_cast<int>(line.find("namespace"));
            symbol.char_end = lines[index].char_start + static_cast<int>(line.size());
            symbol.char_count = symbol.char_end - symbol.char_start;
            result.symbols.push_back(symbol);

            if (line.find('{') != std::string::npos) {
                scopes.push_back({"namespace", symbol.name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
            } else {
                scopes.push_back({"namespace", symbol.name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), 0, line_number});
            }
        } else {
            const auto words = split_words(declaration_before_any(trimmed, "{(:;="));
            auto type_word = std::find_if(words.begin(), words.end(), [](const std::string& word) {
                return word == "class" || word == "interface" || word == "struct" || word == "enum";
            });

            if (type_word != words.end() && std::next(type_word) != words.end()) {
            const auto namespace_name = current_namespace(scopes);
            const auto* parent_type = current_type(scopes);

            CodeSymbol symbol;
            symbol.kind = *type_word;
            symbol.name = *std::next(type_word);
            const auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto base_part = trimmed.substr(colon + 1);
                const auto brace = base_part.find('{');
                if (brace != std::string::npos) {
                    base_part = base_part.substr(0, brace);
                }
                symbol.base_types = split_base_types(base_part);
            }
            symbol.qualified_name = join_qualified(parent_type ? parent_type->qualified_name : namespace_name, symbol.name);
            symbol.signature = normalize_spaces(trimmed);
            symbol.modifiers = modifiers_from_prefix(prefix_before_name(line, symbol.kind));
            symbol.visibility = visibility_from_modifiers(symbol.modifiers);
            symbol.line_start = line_number;
            symbol.line_end = line_number;
            symbol.char_start = lines[index].char_start + static_cast<int>(line.find(symbol.kind));
            symbol.char_end = lines[index].char_start + static_cast<int>(line.size());
            symbol.char_count = symbol.char_end - symbol.char_start;
            symbol.parent_index = parent_type ? parent_type->symbol_index : -1;
            result.symbols.push_back(symbol);

            const int close_depth = line.find('{') != std::string::npos ? brace_depth + 1 : brace_depth + 1;
            scopes.push_back({symbol.kind, symbol.name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), close_depth, line_number});
            } else if (const auto* parent_type = current_type(scopes)) {
            const bool at_member_depth = brace_depth == parent_type->close_depth;
            if (!at_member_depth) {
                brace_depth += count_char(line, '{');
                brace_depth -= count_char(line, '}');

                while (!scopes.empty() && scopes.back().close_depth > 0 && line_number > scopes.back().line_start &&
                    brace_depth < scopes.back().close_depth) {
                    auto& symbol = result.symbols[scopes.back().symbol_index];
                    symbol.line_end = line_number;
                    symbol.char_end = lines[index].char_start + static_cast<int>(lines[index].text.size());
                    symbol.char_count = symbol.char_end - symbol.char_start;
                    scopes.pop_back();
                }
                continue;
            }

            if (line.find('{') != std::string::npos && line.find('(') == std::string::npos) {
                const auto declaration = declaration_before_any(trimmed, "{");
                const auto core_words = without_modifiers(split_words(declaration));
                if (core_words.size() >= 2 && !contains_statement_operator(trimmed) &&
                    is_identifier_like(core_words.back()) && is_identifier_like(core_words[core_words.size() - 2])) {
                CodeSymbol symbol;
                symbol.kind = "property";
                symbol.name = core_words.back();
                symbol.return_type = core_words[core_words.size() - 2];
                symbol.qualified_name = join_qualified(parent_type->qualified_name, symbol.name);
                symbol.signature = normalize_spaces(trimmed);
                symbol.modifiers = modifiers_from_prefix(prefix_before_name(line, symbol.return_type));
                symbol.visibility = visibility_from_modifiers(symbol.modifiers);
                symbol.line_start = line_number;
                symbol.line_end = find_block_end_line(lines, index);
                symbol.char_start = lines[index].char_start + static_cast<int>(line.find(symbol.name));
                symbol.char_end = line_end_char(lines, symbol.line_end);
                symbol.char_count = symbol.char_end - symbol.char_start;
                symbol.parent_index = parent_type->symbol_index;
                result.symbols.push_back(symbol);
                }
            } else if (line.find('(') != std::string::npos && line.find(')') != std::string::npos &&
                !starts_with_statement_keyword(trimmed)) {
                const auto open = line.find('(');
                const auto close = line.find(')', open);
                const auto declaration = trim(line.substr(0, open));
                const auto core_words = without_modifiers(split_words(declaration));
                if (!core_words.empty() && !contains_statement_operator(trimmed) &&
                    (core_words.back() == parent_type->name || core_words.size() >= 2) &&
                    is_identifier_like(core_words.back())) {
                CodeSymbol symbol;
                symbol.name = core_words.back();
                symbol.return_type = core_words.size() >= 2 ? core_words[core_words.size() - 2] : "";
                symbol.kind = symbol.name == parent_type->name ? "constructor" : "method";
                symbol.qualified_name = join_qualified(parent_type->qualified_name, symbol.name);
                symbol.signature = normalize_spaces(trimmed);
                symbol.modifiers = modifiers_from_prefix(prefix_before_name(line, symbol.name));
                symbol.visibility = visibility_from_modifiers(symbol.modifiers);
                symbol.line_start = line_number;
                symbol.line_end = find_block_end_line(lines, index);
                symbol.char_start = lines[index].char_start + static_cast<int>(line.find(symbol.name));
                symbol.char_end = line_end_char(lines, symbol.line_end);
                symbol.char_count = symbol.char_end - symbol.char_start;
                symbol.parent_index = parent_type->symbol_index;
                symbol.parameters = parse_parameters(line.substr(open + 1, close - open - 1));
                result.symbols.push_back(symbol);
                }
            } else if (!starts_with_statement_keyword(trimmed) &&
                (line.find(';') != std::string::npos || line.find('=') != std::string::npos) && line.find('(') == std::string::npos) {
                const auto declaration = declaration_before_any(trimmed, ";=");
                const auto core_words = without_modifiers(split_words(declaration));
                if (core_words.size() >= 2 && !contains_statement_operator(trimmed) &&
                    is_identifier_like(core_words.back()) && is_identifier_like(core_words[core_words.size() - 2])) {
                CodeSymbol symbol;
                symbol.kind = "field";
                symbol.name = core_words.back();
                symbol.return_type = core_words[core_words.size() - 2];
                symbol.qualified_name = join_qualified(parent_type->qualified_name, symbol.name);
                symbol.signature = normalize_spaces(trimmed);
                symbol.modifiers = modifiers_from_prefix(prefix_before_name(line, symbol.return_type));
                symbol.visibility = visibility_from_modifiers(symbol.modifiers);
                symbol.line_start = line_number;
                symbol.line_end = line_number;
                symbol.char_start = lines[index].char_start + static_cast<int>(line.find(symbol.name));
                symbol.char_end = lines[index].char_start + static_cast<int>(line.size());
                symbol.char_count = symbol.char_end - symbol.char_start;
                symbol.parent_index = parent_type->symbol_index;
                result.symbols.push_back(symbol);
                }
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
            symbol.char_count = symbol.char_end - symbol.char_start;
            scopes.pop_back();
        }
    }

    for (auto& symbol : result.symbols) {
        finish_symbol_line_range(symbol, static_cast<int>(lines.size()));
    }

    return result;
}

} // namespace repolens
