#include "repolens/interpreters/jvm_interpreter.hpp"

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
        throw std::runtime_error("Failed to read JVM file: " + path);
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
        if (line[index] == '"') {
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
    return parent.empty() ? name : parent + "." + name;
}

std::vector<repolens::CodeParameter> parse_parameters(const std::string& text, bool kotlin)
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
        const auto declaration = trim(equals == std::string::npos ? part : part.substr(0, equals));
        if (equals != std::string::npos) {
            parameter.default_value = trim(part.substr(equals + 1));
        }
        if (kotlin) {
            const auto colon = declaration.find(':');
            parameter.name = trim(colon == std::string::npos ? declaration : declaration.substr(0, colon));
            parameter.type = colon == std::string::npos ? "" : trim(declaration.substr(colon + 1));
        } else {
            std::stringstream declaration_stream{declaration};
            std::vector<std::string> words;
            std::string word;
            while (declaration_stream >> word) {
                words.push_back(word);
            }
            if (!words.empty()) {
                parameter.name = words.back();
                words.pop_back();
                for (std::size_t index = 0; index < words.size(); ++index) {
                    if (index > 0) {
                        parameter.type += " ";
                    }
                    parameter.type += words[index];
                }
            }
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
        if (scope->kind.find("_class") != std::string::npos || scope->kind.find("_interface") != std::string::npos ||
            scope->kind.find("_enum") != std::string::npos || scope->kind.find("_record") != std::string::npos ||
            scope->kind == "kotlin_object") {
            return &*scope;
        }
    }
    return nullptr;
}

bool starts_with_text(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

void push_contains_relation(repolens::ParseResult& result, const repolens::CodeSymbol& parent, const repolens::CodeSymbol& child)
{
    repolens::SymbolRelation relation;
    relation.source_symbol_name = parent.qualified_name;
    relation.target_symbol_name = child.qualified_name;
    relation.relation_type = "contains";
    relation.source_text = parent.name;
    relation.target_text = child.name;
    result.relations.push_back(relation);
}

void parse_java(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string package_name;
    std::vector<Scope> scopes;
    int brace_depth = 0;
    static const std::regex package_regex{R"(^\s*package\s+([A-Za-z_][A-Za-z0-9_.]*))"};
    static const std::regex import_regex{R"(^\s*import\s+(?:static\s+)?([A-Za-z_][A-Za-z0-9_.*]*))"};
    static const std::regex type_regex{R"(^\s*(?:(?:public|private|protected|abstract|final|static|sealed|non-sealed)\s+)*(class|interface|enum|record)\s+([A-Za-z_][A-Za-z0-9_]*)(.*))"};
    static const std::regex method_regex{R"(^\s*(?:(?:public|private|protected|static|final|abstract|synchronized|native|default|strictfp)\s+)*([A-Za-z_][A-Za-z0-9_<>,.? \[\]]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:throws\s+[^{]+)?\{?)"};
    static const std::regex ctor_regex{R"(^\s*(?:(?:public|private|protected)\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*\{?)"};
    static const std::regex field_regex{R"(^\s*(?:(?:public|private|protected|static|final|volatile|transient)\s+)*([A-Za-z_][A-Za-z0-9_<>,.? \[\]]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=.*)?;)"}; 

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        if (trimmed.empty() || starts_with_text(trimmed, "*") || starts_with_text(trimmed, "/*")) {
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
        if (std::regex_search(trimmed, match, package_regex)) {
            package_name = match[1].str();
            result.symbols.push_back(make_symbol("java_package", package_name, package_name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, import_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("java_import", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto kind = std::string{"java_"} + match[1].str();
            const auto name = match[2].str();
            const auto parent = current_type(scopes);
            const auto qualified = join_qualified(parent ? parent->qualified_name : package_name, name);
            auto symbol = make_symbol(kind, name, qualified, trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            if (parent) {
                push_contains_relation(result, result.symbols[static_cast<std::size_t>(parent->symbol_index)], result.symbols.back());
            }
            scopes.push_back({kind, name, qualified, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (const auto* parent = current_type(scopes); parent && std::regex_search(trimmed, match, ctor_regex) && match[1].str() == parent->name) {
            auto symbol = make_symbol("java_constructor", parent->name, join_qualified(parent->qualified_name, parent->name), trimmed, lines[index], line_number, parent->symbol_index);
            symbol.parameters = parse_parameters(match[2].str(), false);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            push_contains_relation(result, result.symbols[static_cast<std::size_t>(parent->symbol_index)], result.symbols.back());
        } else if (const auto* parent = current_type(scopes); parent && std::regex_search(trimmed, match, method_regex)) {
            const auto name = match[2].str();
            auto symbol = make_symbol("java_method", name, join_qualified(parent->qualified_name, name), trimmed, lines[index], line_number, parent->symbol_index);
            symbol.return_type = trim(match[1].str());
            symbol.parameters = parse_parameters(match[3].str(), false);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            push_contains_relation(result, result.symbols[static_cast<std::size_t>(parent->symbol_index)], result.symbols.back());
        } else if (const auto* parent = current_type(scopes); parent && std::regex_search(trimmed, match, field_regex)) {
            const auto name = match[2].str();
            auto symbol = make_symbol("java_field", name, join_qualified(parent->qualified_name, name), trimmed, lines[index], line_number, parent->symbol_index);
            symbol.return_type = trim(match[1].str());
            result.symbols.push_back(symbol);
            push_contains_relation(result, result.symbols[static_cast<std::size_t>(parent->symbol_index)], result.symbols.back());
        }

        brace_depth += count_char(line, '{') - count_char(line, '}');
    }
}

void parse_kotlin(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string package_name;
    std::vector<Scope> scopes;
    int brace_depth = 0;
    static const std::regex package_regex{R"(^\s*package\s+([A-Za-z_][A-Za-z0-9_.]*))"};
    static const std::regex import_regex{R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_.*]*))"};
    static const std::regex type_regex{R"(^\s*(?:(?:data|sealed|open|abstract|inner|enum)\s+)*(class|interface|object)\s+([A-Za-z_][A-Za-z0-9_]*)(.*))"};
    static const std::regex function_regex{R"(^\s*(?:(?:public|private|protected|internal|open|override|suspend|inline|operator)\s+)*fun\s+(?:[A-Za-z_][A-Za-z0-9_.<>]*\.)?([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?::\s*([A-Za-z_][A-Za-z0-9_.<>?]*))?)"};
    static const std::regex property_regex{R"(^\s*(?:(?:public|private|protected|internal|override|lateinit|const)\s+)*(val|var)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([^=]+))?)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        if (trimmed.empty()) {
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
        if (std::regex_search(trimmed, match, package_regex)) {
            package_name = match[1].str();
            result.symbols.push_back(make_symbol("kotlin_package", package_name, package_name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, import_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("kotlin_import", name, name, trimmed, lines[index], line_number));
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto raw_kind = match[1].str();
            const auto kind = raw_kind == "object" ? "kotlin_object" : std::string{"kotlin_"} + raw_kind;
            const auto name = match[2].str();
            const auto* parent = current_type(scopes);
            const auto qualified = join_qualified(parent ? parent->qualified_name : package_name, name);
            auto symbol = make_symbol(kind, name, qualified, trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, qualified, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
        } else if (std::regex_search(trimmed, match, function_regex)) {
            const auto name = match[1].str();
            const auto* parent = current_type(scopes);
            auto symbol = make_symbol(parent ? "kotlin_method" : "kotlin_function", name, join_qualified(parent ? parent->qualified_name : package_name, name), trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            symbol.parameters = parse_parameters(match[2].str(), true);
            if (match.size() > 3) {
                symbol.return_type = trim(match[3].str());
            }
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, property_regex)) {
            const auto name = match[2].str();
            const auto* parent = current_type(scopes);
            auto symbol = make_symbol(parent ? "kotlin_property" : "kotlin_variable", name, join_qualified(parent ? parent->qualified_name : package_name, name), trimmed, lines[index], line_number, parent ? parent->symbol_index : -1);
            if (match.size() > 3) {
                symbol.return_type = trim(match[3].str());
            }
            result.symbols.push_back(symbol);
        }

        brace_depth += count_char(line, '{') - count_char(line, '}');
    }
}

std::string xml_text_value(const std::string& line, const std::string& tag)
{
    const auto open = line.find("<" + tag + ">");
    const auto close = line.find("</" + tag + ">");
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return "";
    }
    return trim(line.substr(open + tag.size() + 2, close - open - tag.size() - 2));
}

void parse_pom(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    bool in_dependency = false;
    std::string group_id;
    std::string artifact_id;
    int dependency_line = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        const int line_number = static_cast<int>(index + 1);
        if (trimmed.find("<dependency>") != std::string::npos) {
            in_dependency = true;
            group_id.clear();
            artifact_id.clear();
            dependency_line = line_number;
            continue;
        }
        if (in_dependency) {
            const auto group = xml_text_value(trimmed, "groupId");
            const auto artifact = xml_text_value(trimmed, "artifactId");
            if (!group.empty()) {
                group_id = group;
            }
            if (!artifact.empty()) {
                artifact_id = artifact;
            }
            if (trimmed.find("</dependency>") != std::string::npos) {
                const auto name = group_id.empty() ? artifact_id : group_id + ":" + artifact_id;
                if (!artifact_id.empty()) {
                    result.symbols.push_back(make_symbol("maven_dependency", name, name, name, lines[static_cast<std::size_t>(dependency_line - 1)], dependency_line));
                }
                in_dependency = false;
            }
        } else {
            const auto artifact = xml_text_value(trimmed, "artifactId");
            if (!artifact.empty()) {
                result.symbols.push_back(make_symbol("maven_artifact", artifact, artifact, trimmed, lines[index], line_number));
            }
        }
    }
}

void parse_gradle(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    static const std::regex dependency_regex{R"((?:implementation|api|compileOnly|runtimeOnly|testImplementation|classpath)\s*\(?\s*['"]([^'"]+)['"])"}; 
    static const std::regex plugin_regex{R"(id\s+['"]([^'"]+)['"])"};
    static const std::regex task_regex{R"(\btask\s+([A-Za-z_][A-Za-z0-9_]*)|tasks\.register\s*\(\s*['"]([^'"]+)['"])"}; 
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(strip_line_comment(lines[index].text));
        const int line_number = static_cast<int>(index + 1);
        std::smatch match;
        if (std::regex_search(trimmed, match, dependency_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("gradle_dependency", name, name, trimmed, lines[index], line_number));
        }
        if (std::regex_search(trimmed, match, plugin_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("gradle_plugin", name, name, trimmed, lines[index], line_number));
        }
        if (std::regex_search(trimmed, match, task_regex)) {
            const auto name = match[1].matched ? match[1].str() : match[2].str();
            result.symbols.push_back(make_symbol("gradle_task", name, name, trimmed, lines[index], line_number));
        }
    }
}

} // namespace

namespace repolens {

std::string JvmInterpreter::language_id() const
{
    return "jvm";
}

std::vector<std::string> JvmInterpreter::file_extensions() const
{
    return {".java", ".kt", ".kts", ".gradle", "pom.xml", "build.gradle", "settings.gradle", "build.gradle.kts", "settings.gradle.kts"};
}

ParseResult JvmInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto extension = extension_for(file);
    const auto filename = filename_for(file);

    if (filename == "pom.xml") {
        result.language = "maven";
        parse_pom(lines, result);
    } else if (extension == ".gradle" || filename == "build.gradle.kts" || filename == "settings.gradle.kts") {
        result.language = "gradle";
        parse_gradle(lines, result);
    } else if (extension == ".kt" || extension == ".kts") {
        result.language = "kotlin";
        parse_kotlin(lines, result);
    } else {
        result.language = "java";
        parse_java(lines, result);
    }

    return result;
}

} // namespace repolens
