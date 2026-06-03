#include "repolens/interpreters/python_interpreter.hpp"

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
    int indent = 0;
    int line_start = 0;
};

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("Failed to read Python file: " + path);
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

int indentation(const std::string& line)
{
    int count = 0;
    for (char c : line) {
        if (c == ' ') {
            ++count;
        } else if (c == '\t') {
            count += 4;
        } else {
            break;
        }
    }
    return count;
}

std::string join_qualified(const std::string& parent, const std::string& name)
{
    return parent.empty() ? name : parent + "." + name;
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

std::string parent_qualified_name(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "python_class") {
            return scope->qualified_name;
        }
    }
    return "";
}

const Scope* current_class(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "python_class") {
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
        if (part.empty()) {
            continue;
        }

        const auto equals = part.find('=');
        auto declaration = trim(equals == std::string::npos ? part : part.substr(0, equals));
        const auto colon = declaration.find(':');

        repolens::CodeParameter parameter;
        parameter.position = position++;
        parameter.name = trim(colon == std::string::npos ? declaration : declaration.substr(0, colon));
        if (colon != std::string::npos) {
            parameter.type = trim(declaration.substr(colon + 1));
        }
        if (equals != std::string::npos) {
            parameter.default_value = trim(part.substr(equals + 1));
        }
        parameters.push_back(parameter);
    }
    return parameters;
}

int find_python_block_end_line(const std::vector<LineInfo>& lines, std::size_t start_index, int start_indent)
{
    int last_line = static_cast<int>(start_index + 1);
    for (std::size_t index = start_index + 1; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (indentation(lines[index].text) <= start_indent) {
            break;
        }
        last_line = static_cast<int>(index + 1);
    }
    return last_line;
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
    symbol.char_start = line.char_start + static_cast<int>(name_position == std::string::npos ? indentation(line.text) : name_position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    symbol.parent_index = parent_index;
    return symbol;
}

void close_scopes_for_indent(std::vector<Scope>& scopes, std::vector<repolens::CodeSymbol>& symbols, const std::vector<LineInfo>& lines, int line_number, int indent)
{
    while (!scopes.empty() && line_number > scopes.back().line_start && indent <= scopes.back().indent) {
        auto& symbol = symbols[static_cast<std::size_t>(scopes.back().symbol_index)];
        symbol.line_end = std::max(symbol.line_start, line_number - 1);
        symbol.char_end = line_end_char(lines, symbol.line_end);
        symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
        scopes.pop_back();
    }
}

std::string clean_requirement_name(std::string text)
{
    text = trim(text);
    if (text.empty() || text.front() == '#' || text.front() == '-') {
        return "";
    }
    const auto marker = text.find_first_of("<>=!~[ ;");
    if (marker != std::string::npos) {
        text = text.substr(0, marker);
    }
    return trim(text);
}

std::string clean_dependency_name(std::string text)
{
    text = trim(text);
    if (!text.empty() && (text.front() == '"' || text.front() == '\'')) {
        text.erase(text.begin());
    }
    if (!text.empty() && (text.back() == '"' || text.back() == '\'' || text.back() == ',')) {
        text.pop_back();
    }
    return clean_requirement_name(text);
}

std::vector<std::string> quoted_values(const std::string& text)
{
    std::vector<std::string> values;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto quote = text.find_first_of("\"'", cursor);
        if (quote == std::string::npos) {
            break;
        }
        const char quote_char = text[quote];
        const auto close = text.find(quote_char, quote + 1);
        if (close == std::string::npos) {
            break;
        }
        auto value = clean_dependency_name(text.substr(quote + 1, close - quote - 1));
        if (!value.empty()) {
            values.push_back(value);
        }
        cursor = close + 1;
    }
    return values;
}

void parse_python_source(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<Scope> scopes;
    std::vector<std::string> decorators;
    const std::regex class_regex{R"(^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(([^)]*)\))?\s*:)"};
    const std::regex function_regex{R"(^\s*(async\s+)?def\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:->\s*([^:]+))?\s*:)"};
    const std::regex assignment_regex{R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([^=]+))?\s*=)"};
    const std::regex import_regex{R"(^\s*(?:from\s+([A-Za-z0-9_\.]+)\s+import\s+(.+)|import\s+(.+)))"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const int indent = indentation(lines[index].text);
        close_scopes_for_indent(scopes, result.symbols, lines, line_number, indent);

        if (trimmed.front() == '@') {
            decorators.push_back(trimmed);
            continue;
        }

        std::smatch match;
        if (std::regex_search(lines[index].text, match, class_regex)) {
            const auto name = match[1].str();
            const auto parent = parent_qualified_name(scopes);
            auto symbol = make_symbol("python_class", name, join_qualified(parent, name), trimmed, lines[index], line_number, current_class(scopes) ? current_class(scopes)->symbol_index : -1);
            if (match.size() > 2) {
                symbol.return_type = trim(match[2].str());
            }
            symbol.line_end = find_python_block_end_line(lines, index, indent);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            scopes.push_back({"python_class", name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), indent, line_number});
        } else if (std::regex_search(lines[index].text, match, function_regex)) {
            const auto name = match[2].str();
            const auto* parent_class = current_class(scopes);
            const bool is_async = match[1].matched;
            std::string kind = parent_class ? (is_async ? "python_async_method" : "python_method") :
                (is_async ? "python_async_function" : "python_function");
            for (const auto& decorator : decorators) {
                if (decorator.find(".route") != std::string::npos || decorator.find("@route") != std::string::npos) {
                    kind = "python_route";
                }
            }
            auto symbol = make_symbol(kind, name, join_qualified(parent_class ? parent_class->qualified_name : "", name), trimmed, lines[index], line_number, parent_class ? parent_class->symbol_index : -1);
            symbol.parameters = parse_parameters(match[3].str());
            if (match.size() > 4) {
                symbol.return_type = trim(match[4].str());
            }
            symbol.modifiers = is_async ? "async" : "";
            for (const auto& decorator : decorators) {
                if (!symbol.modifiers.empty()) {
                    symbol.modifiers += " ";
                }
                symbol.modifiers += decorator;
            }
            symbol.line_end = find_python_block_end_line(lines, index, indent);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(lines[index].text, match, import_regex)) {
            const auto module = match[1].matched ? match[1].str() : match[3].str();
            const auto name = module.empty() ? "import" : module;
            result.symbols.push_back(make_symbol("python_import", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(lines[index].text, match, assignment_regex) && indent == 0) {
            const auto name = match[1].str();
            auto symbol = make_symbol("python_variable", name, name, trimmed, lines[index], line_number);
            if (match.size() > 2) {
                symbol.return_type = trim(match[2].str());
            }
            result.symbols.push_back(symbol);
        } else if (std::regex_search(lines[index].text, match, assignment_regex)) {
            if (const auto* parent_class = current_class(scopes); parent_class && indent > parent_class->indent) {
                const auto name = match[1].str();
                auto symbol = make_symbol("python_field", name, join_qualified(parent_class->qualified_name, name), trimmed, lines[index], line_number, parent_class->symbol_index);
                if (match.size() > 2) {
                    symbol.return_type = trim(match[2].str());
                }
                result.symbols.push_back(symbol);
            }
        }

        if (trimmed.front() != '@') {
            decorators.clear();
        }
    }

    close_scopes_for_indent(scopes, result.symbols, lines, static_cast<int>(lines.size()) + 1, 0);
}

void parse_requirements(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto name = clean_requirement_name(lines[index].text);
        if (!name.empty()) {
            result.symbols.push_back(make_symbol("python_dependency", name, name, trim(lines[index].text), lines[index], static_cast<int>(index + 1)));
        }
    }
}

void parse_ini_like(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    int section_index = -1;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const int line_number = static_cast<int>(index + 1);
        if (trimmed.front() == '[' && trimmed.find(']') != std::string::npos) {
            section = trimmed.substr(1, trimmed.find(']') - 1);
            result.symbols.push_back(make_symbol("python_config_section", section, section, trimmed, lines[index], line_number));
            section_index = static_cast<int>(result.symbols.size() - 1);
            continue;
        }
        const auto equals = trimmed.find('=');
        if (equals != std::string::npos) {
            const auto name = trim(trimmed.substr(0, equals));
            if (!name.empty()) {
                result.symbols.push_back(make_symbol("python_config_property", name, section.empty() ? name : section + "." + name, trimmed, lines[index], line_number, section_index));
            }
        }
    }
}

void parse_pyproject_toml(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    int section_index = -1;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const int line_number = static_cast<int>(index + 1);
        if (trimmed.front() == '[' && trimmed.find(']') != std::string::npos) {
            section = trimmed.substr(trimmed.find('[') + 1, trimmed.find(']') - trimmed.find('[') - 1);
            result.symbols.push_back(make_symbol("python_config_section", section, section, trimmed, lines[index], line_number));
            section_index = static_cast<int>(result.symbols.size() - 1);
            continue;
        }

        const auto equals = trimmed.find('=');
        if (equals != std::string::npos) {
            const auto name = trim(trimmed.substr(0, equals));
            if (name.empty()) {
                continue;
            }
            const bool dependency_section = section.find("dependencies") != std::string::npos ||
                section == "project.optional-dependencies" || section == "tool.poetry.dependencies" ||
                section == "packages" || section == "dev-packages" || section == "requires";
            const auto kind = dependency_section ? "python_dependency" : "python_config_property";
            result.symbols.push_back(make_symbol(kind, name, section.empty() ? name : section + "." + name, trimmed, lines[index], line_number, section_index));

            if (name == "dependencies" || name == "requires" || name == "install_requires") {
                for (const auto& dependency : quoted_values(trimmed.substr(equals + 1))) {
                    result.symbols.push_back(make_symbol("python_dependency", dependency, dependency, trimmed, lines[index], line_number, section_index));
                }
            }
        }
    }
}

} // namespace

namespace repolens {

std::string PythonInterpreter::language_id() const
{
    return "python";
}

std::vector<std::string> PythonInterpreter::file_extensions() const
{
    return {
        ".py", ".pyw", ".pyi",
        "requirements.txt", "constraints.txt", "pyproject.toml", "setup.cfg",
        "tox.ini", "pytest.ini", "mypy.ini", "Pipfile", "poetry.lock"};
}

ParseResult PythonInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto filename = filename_for(file);
    const auto extension = extension_for(file);

    if (extension == ".py" || extension == ".pyw" || extension == ".pyi") {
        result.language = "python";
        parse_python_source(lines, result);
    } else if (filename == "requirements.txt" || filename == "constraints.txt") {
        result.language = "python-requirements";
        parse_requirements(lines, result);
    } else if (filename == "pyproject.toml" || filename == "pipfile" || filename == "poetry.lock") {
        result.language = "python-toml";
        parse_pyproject_toml(lines, result);
    } else {
        result.language = "python-config";
        parse_ini_like(lines, result);
    }

    return result;
}

} // namespace repolens
