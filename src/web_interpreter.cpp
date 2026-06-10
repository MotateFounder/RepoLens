#include "repolens/interpreters/web_interpreter.hpp"

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
        throw std::runtime_error("Failed to read web file: " + path);
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

std::string strip_line_comment(const std::string& text)
{
    const auto position = text.find("//");
    return position == std::string::npos ? text : text.substr(0, position);
}

int count_char(const std::string& text, char value)
{
    return static_cast<int>(std::count(text.begin(), text.end(), value));
}

bool starts_with_upper(const std::string& text)
{
    return !text.empty() && std::isupper(static_cast<unsigned char>(text.front())) != 0;
}

bool starts_with_text(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
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

std::string extension_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.extension().string());
}

std::string filename_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.filename().string());
}

std::string language_for(const repolens::FileMetadata& file)
{
    const auto extension = extension_for(file);
    if (extension == ".ts" || extension == ".tsx") {
        return "typescript";
    }
    if (extension == ".jsx") {
        return "jsx";
    }
    if (extension == ".vue") {
        return "vue";
    }
    if (extension == ".html" || extension == ".htm") {
        return "html";
    }
    if (extension == ".css" || extension == ".scss" || extension == ".sass" || extension == ".less") {
        return "css";
    }
    if (filename_for(file) == "package.json") {
        return "npm";
    }
    if (extension == ".json") {
        return "json";
    }
    return "javascript";
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
        const auto colon = part.find(':');
        repolens::CodeParameter parameter;
        parameter.position = position++;
        parameter.name = trim(colon == std::string::npos ? part : part.substr(0, colon));
        if (colon != std::string::npos) {
            parameter.type = trim(part.substr(colon + 1));
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

std::string current_parent(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "js_class" || scope->kind == "react_component" || scope->kind == "angular_component" ||
            scope->kind == "angular_service" || scope->kind == "angular_module") {
            return scope->qualified_name;
        }
    }
    return "";
}

const Scope* current_class_scope(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "js_class" || scope->kind == "react_component" || scope->kind == "angular_component" ||
            scope->kind == "angular_service" || scope->kind == "angular_module") {
            return &*scope;
        }
    }
    return nullptr;
}

bool is_js_statement_keyword(const std::string& name)
{
    return name == "if" || name == "for" || name == "while" || name == "switch" || name == "catch" ||
        name == "return" || name == "function" || name == "else";
}

void parse_script_like(const repolens::FileMetadata& file, const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    (void)file;
    std::vector<Scope> scopes;
    int brace_depth = 0;
    std::string pending_angular_kind;

    static const std::regex class_regex{R"(^\s*(?:export\s+default\s+|export\s+)?class\s+([A-Za-z_$][A-Za-z0-9_$]*))"};
    static const std::regex function_regex{R"(^\s*(?:export\s+)?(?:async\s+)?function\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*\(([^)]*)\))"};
    static const std::regex arrow_regex{R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*[:A-Za-z0-9_<>,\s\[\]\|&?]*=\s*(?:async\s*)?\(([^)]*)\)\s*=>)"};
    static const std::regex type_regex{R"(^\s*(?:export\s+)?(interface|type|enum)\s+([A-Za-z_$][A-Za-z0-9_$]*))"};
    static const std::regex method_regex{R"(^\s*(?:async\s+)?([A-Za-z_$][A-Za-z0-9_$]*)\s*\(([^)]*)\)\s*\{?)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto line = strip_line_comment(lines[index].text);
        const auto trimmed = trim(line);
        std::smatch match;

        if (trimmed.empty()) {
            continue;
        }

        if (starts_with_text(trimmed, "@Component")) {
            pending_angular_kind = "angular_component";
        } else if (starts_with_text(trimmed, "@Injectable")) {
            pending_angular_kind = "angular_service";
        } else if (starts_with_text(trimmed, "@NgModule")) {
            pending_angular_kind = "angular_module";
        } else if (starts_with_text(trimmed, "@Directive")) {
            pending_angular_kind = "angular_directive";
        } else if (starts_with_text(trimmed, "@Pipe")) {
            pending_angular_kind = "angular_pipe";
        }

        if (std::regex_search(trimmed, match, class_regex)) {
            const auto name = match[1].str();
            std::string kind = pending_angular_kind.empty() ? "js_class" : pending_angular_kind;
            if (pending_angular_kind.empty() && starts_with_upper(name) && trimmed.find("extends React") != std::string::npos) {
                kind = "react_component";
            }
            auto symbol = make_symbol(kind, name, name, trimmed, lines[index], line_number);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, name, static_cast<int>(result.symbols.size() - 1), brace_depth + 1, line_number});
            pending_angular_kind.clear();
        } else if (std::regex_search(trimmed, match, function_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol(starts_with_upper(name) ? "react_component" : (starts_with_text(name, "use") ? "react_hook" : "js_function"), name, name, trimmed, lines[index], line_number);
            symbol.parameters = parse_parameters(match[2].str());
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, arrow_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol(starts_with_upper(name) ? "react_component" : (starts_with_text(name, "use") ? "react_hook" : "js_function"), name, name, trimmed, lines[index], line_number);
            symbol.parameters = parse_parameters(match[2].str());
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (std::regex_search(trimmed, match, type_regex)) {
            const auto kind = std::string{"ts_"} + match[1].str();
            const auto name = match[2].str();
            auto symbol = make_symbol(kind, name, name, trimmed, lines[index], line_number);
            symbol.line_end = find_block_end_line(lines, index);
            symbol.char_end = line_end_char(lines, symbol.line_end);
            symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
            result.symbols.push_back(symbol);
        } else if (const auto* parent = current_class_scope(scopes);
            parent && brace_depth == parent->close_depth && std::regex_search(trimmed, match, method_regex)) {
            const auto name = match[1].str();
            if (!is_js_statement_keyword(name)) {
                auto symbol = make_symbol("js_method", name, join_qualified(parent->qualified_name, name), trimmed, lines[index], line_number, parent->symbol_index);
                symbol.parameters = parse_parameters(match[2].str());
                symbol.line_end = find_block_end_line(lines, index);
                symbol.char_end = line_end_char(lines, symbol.line_end);
                symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
                result.symbols.push_back(symbol);
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
}

std::string attribute_value(const std::string& line, const std::string& name)
{
    auto position = line.find(name + "=");
    if (position == std::string::npos) {
        return "";
    }
    position = line.find_first_of("\"'", position + name.size());
    if (position == std::string::npos) {
        return "";
    }
    const char quote = line[position];
    const auto end = line.find(quote, position + 1);
    return end == std::string::npos ? "" : line.substr(position + 1, end - position - 1);
}

std::string tag_name(const std::string& line)
{
    const auto open = line.find('<');
    if (open == std::string::npos || open + 1 >= line.size() || line[open + 1] == '/' || line[open + 1] == '!' || line[open + 1] == '?') {
        return "";
    }
    auto end = open + 1;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '>' && line[end] != '/') {
        ++end;
    }
    return line.substr(open + 1, end - open - 1);
}

void parse_markup(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        const auto tag = tag_name(trimmed);
        if (tag.empty()) {
            continue;
        }
        const int line_number = static_cast<int>(index + 1);
        const auto id = attribute_value(trimmed, "id");
        if (!id.empty()) {
            auto symbol = make_symbol("html_id", id, id, trimmed, lines[index], line_number);
            symbol.return_type = tag;
            result.symbols.push_back(symbol);
        }
        const auto class_list = attribute_value(trimmed, "class");
        std::stringstream classes{class_list};
        std::string class_name;
        while (classes >> class_name) {
            auto symbol = make_symbol("html_class", class_name, class_name, trimmed, lines[index], line_number);
            symbol.return_type = tag;
            result.symbols.push_back(symbol);
        }
    }
}

void parse_stylesheet(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        const int line_number = static_cast<int>(index + 1);
        if (starts_with_text(trimmed, "@keyframes")) {
            std::stringstream stream{trimmed};
            std::string at;
            std::string name;
            stream >> at >> name;
            if (!name.empty()) {
                result.symbols.push_back(make_symbol("css_keyframes", name, name, trimmed, lines[index], line_number));
            }
        } else if (trimmed.find('{') != std::string::npos) {
            auto selector_text = trim(trimmed.substr(0, trimmed.find('{')));
            std::stringstream selectors{selector_text};
            std::string selector;
            while (std::getline(selectors, selector, ',')) {
                selector = trim(selector);
                if (!selector.empty()) {
                    result.symbols.push_back(make_symbol("css_selector", selector, selector, trimmed, lines[index], line_number));
                }
            }
        } else if (starts_with_text(trimmed, "--")) {
            const auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                const auto name = trim(trimmed.substr(0, colon));
                result.symbols.push_back(make_symbol("css_custom_property", name, name, trimmed, lines[index], line_number));
            }
        }
    }
}

void parse_package_json(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    std::vector<std::string> object_stack;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto trimmed = trim(lines[index].text);
        auto scan_text = trimmed;
        const int line_number = static_cast<int>(index + 1);

        if (trimmed.size() > 2 && trimmed.front() == '"') {
            const auto key_end = trimmed.find('"', 1);
            if (key_end != std::string::npos) {
                const auto key = trimmed.substr(1, key_end - 1);
                auto after_key = key_end + 1;
                while (after_key < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[after_key]))) {
                    ++after_key;
                }

                if (after_key < trimmed.size() && trimmed[after_key] == ':') {
                    auto value = trim(trimmed.substr(after_key + 1));
                    if (!value.empty() && value.back() == ',') {
                        value.pop_back();
                        value = trim(value);
                    }

                    std::string parent;
                    for (std::size_t stack_index = 0; stack_index < object_stack.size(); ++stack_index) {
                        if (stack_index > 0) {
                            parent += ".";
                        }
                        parent += object_stack[stack_index];
                    }
                    const auto qualified = parent.empty() ? key : parent + "." + key;

                    const bool is_object_value = starts_with_text(value, "{") || starts_with_text(value, "[");
                    auto symbol = make_symbol(is_object_value ? "json_object" : "json_property", key, qualified, trimmed, lines[index], line_number);
                    symbol.return_type = value;
                    result.symbols.push_back(symbol);

                    if (is_object_value && value.find('}') == std::string::npos && value.find(']') == std::string::npos) {
                        object_stack.push_back(key);
                    }
                }
            }
        }

        if (trimmed.find("\"scripts\"") != std::string::npos) {
            section = "npm_script";
            const auto open = trimmed.find('{');
            scan_text = open == std::string::npos ? "" : trimmed.substr(open + 1);
        }
        if (trimmed.find("\"dependencies\"") != std::string::npos || trimmed.find("\"devDependencies\"") != std::string::npos) {
            section = "npm_dependency";
            const auto open = trimmed.find('{');
            scan_text = open == std::string::npos ? "" : trimmed.substr(open + 1);
        }

        if (!section.empty()) {
            std::size_t cursor = 0;
            while (true) {
                const auto start = scan_text.find('"', cursor);
                if (start == std::string::npos) {
                    break;
                }
                const auto end = scan_text.find('"', start + 1);
                if (end == std::string::npos) {
                    break;
                }
                auto after_key = end + 1;
                while (after_key < scan_text.size() && std::isspace(static_cast<unsigned char>(scan_text[after_key]))) {
                    ++after_key;
                }
                if (after_key >= scan_text.size() || scan_text[after_key] != ':') {
                    cursor = end + 1;
                    continue;
                }
                const auto name = scan_text.substr(start + 1, end - start - 1);
                result.symbols.push_back(make_symbol(section, name, name, trimmed, lines[index], line_number));
                cursor = after_key + 1;
            }
        }

        if (trimmed.find('}') != std::string::npos) {
            section.clear();
            if (!object_stack.empty()) {
                object_stack.pop_back();
            }
        }
    }
}

void parse_vue(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<LineInfo> script_lines;
    bool in_script = false;
    bool in_style = false;
    bool in_template = false;

    for (const auto& line : lines) {
        const auto lower = lower_text(trim(line.text));
        if (lower.find("<script") != std::string::npos) {
            in_script = true;
            continue;
        }
        if (lower.find("</script>") != std::string::npos) {
            in_script = false;
            continue;
        }
        if (lower.find("<style") != std::string::npos) {
            in_style = true;
            continue;
        }
        if (lower.find("</style>") != std::string::npos) {
            in_style = false;
            continue;
        }
        if (lower.find("<template") != std::string::npos) {
            in_template = true;
            continue;
        }
        if (lower.find("</template>") != std::string::npos) {
            in_template = false;
            continue;
        }

        if (in_script) {
            script_lines.push_back(line);
        } else if (in_style) {
            std::vector<LineInfo> one{line};
            parse_stylesheet(one, result);
        } else if (in_template) {
            std::vector<LineInfo> one{line};
            parse_markup(one, result);
        }
    }

    parse_script_like({}, script_lines, result);
}

} // namespace

namespace repolens {

std::string WebInterpreter::language_id() const
{
    return "web";
}

std::vector<std::string> WebInterpreter::file_extensions() const
{
    return {".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".html", ".htm", ".css", ".scss", ".sass", ".less", ".vue", ".json", "package.json"};
}

ParseResult WebInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    result.language = language_for(file);
    const auto lines = read_lines(file.absolute_path);

    if (result.language == "html") {
        parse_markup(lines, result);
    } else if (result.language == "css") {
        parse_stylesheet(lines, result);
    } else if (result.language == "vue") {
        parse_vue(lines, result);
    } else if (result.language == "npm" || result.language == "json") {
        parse_package_json(lines, result);
    } else {
        parse_script_like(file, lines, result);
    }

    return result;
}

} // namespace repolens
