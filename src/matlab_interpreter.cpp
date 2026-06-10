#include "repolens/interpreters/matlab_interpreter.hpp"

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
        throw std::runtime_error("Failed to read MATLAB/Octave/Scilab file: " + path);
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
    bool in_single_quote = false;
    bool in_double_quote = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char c = line[index];
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (c == '%' && !in_single_quote && !in_double_quote) {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string strip_scilab_comment(const std::string& line)
{
    bool in_single_quote = false;
    bool in_double_quote = false;
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        const char c = line[index];
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        } else if (c == '/' && line[index + 1] == '/' && !in_single_quote && !in_double_quote) {
            return line.substr(0, index);
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
    return parent.empty() ? name : parent + "." + name;
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
        parameter.name = part;
        parameters.push_back(parameter);
    }
    return parameters;
}

std::string clean_name(std::string name)
{
    name = trim(name);
    while (!name.empty() && (name.back() == ';' || name.back() == ',')) {
        name.pop_back();
        name = trim(name);
    }
    return name;
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
    symbol.name = clean_name(name);
    symbol.qualified_name = qualified_name.empty() ? symbol.name : qualified_name;
    symbol.signature = normalize_spaces(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto position = line.text.find(symbol.name);
    symbol.char_start = line.char_start + static_cast<int>(position == std::string::npos ? 0 : position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    symbol.parent_index = parent_index;
    return symbol;
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

const Scope* current_class(const std::vector<Scope>& scopes)
{
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (scope->kind == "matlab_class") {
            return &*scope;
        }
    }
    return nullptr;
}

bool inside_function_or_method(const std::vector<Scope>& scopes)
{
    for (const auto& scope : scopes) {
        if (scope.kind == "matlab_function" || scope.kind == "matlab_method" || scope.kind == "scilab_function") {
            return true;
        }
    }
    return false;
}

bool inside_properties_block(const std::vector<Scope>& scopes)
{
    return !scopes.empty() && scopes.back().kind == "matlab_properties_block";
}

int current_parent_index(const std::vector<Scope>& scopes)
{
    if (const auto* scope = current_class(scopes)) {
        return scope->symbol_index;
    }
    return -1;
}

std::string current_parent_name(const std::vector<Scope>& scopes)
{
    if (const auto* scope = current_class(scopes)) {
        return scope->qualified_name;
    }
    return "";
}

void parse_matlab_or_octave(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<Scope> scopes;
    static const std::regex class_regex{R"(^\s*classdef(?:\s*\([^)]*\))?\s+([A-Za-z]\w*)(?:\s*<\s*(.+))?)"};
    static const std::regex function_regex{R"(^\s*function\s+(?:(?:\[[^\]]+\]|[A-Za-z]\w*)\s*=\s*)?([A-Za-z]\w*)\s*(?:\(([^)]*)\))?)"};
    static const std::regex property_regex{R"(^\s*([A-Za-z]\w*)\s*(?:\([^)]*\))?\s*(?:=.*)?$)"};
    static const std::regex assignment_regex{R"(^\s*([A-Za-z]\w*)\s*=)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto raw_trimmed = trim(lines[index].text);
        if (raw_trimmed.empty()) {
            continue;
        }
        if (raw_trimmed.rfind("%%", 0) == 0) {
            const auto name = trim(raw_trimmed.substr(2));
            if (!name.empty()) {
                result.symbols.push_back(make_symbol("matlab_section", name, name, raw_trimmed, lines[index], line_number));
            }
            continue;
        }
        if (raw_trimmed.front() == '%') {
            continue;
        }

        const auto code = trim(strip_comment(lines[index].text));
        if (code.empty()) {
            continue;
        }

        const auto lower = lower_text(code);
        std::smatch match;
        if (std::regex_search(code, match, class_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("matlab_class", name, name, code, lines[index], line_number);
            if (match.size() > 2) {
                symbol.return_type = trim(match[2].str());
            }
            result.symbols.push_back(symbol);
            scopes.push_back({"matlab_class", name, name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (lower.rfind("properties", 0) == 0) {
            scopes.push_back({"matlab_properties_block", "properties", current_parent_name(scopes), current_parent_index(scopes), line_number});
        } else if (lower == "end" || lower.rfind("end ", 0) == 0) {
            close_scope(scopes, result.symbols, lines, line_number);
        } else if (std::regex_search(code, match, function_regex)) {
            const auto name = match[1].str();
            const auto parent = current_parent_name(scopes);
            const auto kind = parent.empty() ? "matlab_function" : "matlab_method";
            auto symbol = make_symbol(kind, name, join_qualified(parent, name), code, lines[index], line_number, current_parent_index(scopes));
            if (match.size() > 2) {
                symbol.parameters = parse_parameters(match[2].str());
            }
            result.symbols.push_back(symbol);
            scopes.push_back({kind, name, symbol.qualified_name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (inside_properties_block(scopes) && std::regex_search(code, match, property_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("matlab_property", name, join_qualified(current_parent_name(scopes), name), code, lines[index], line_number, current_parent_index(scopes)));
        } else if (!inside_function_or_method(scopes) && std::regex_search(code, match, assignment_regex)) {
            const auto name = match[1].str();
            const auto parent = current_parent_name(scopes);
            result.symbols.push_back(make_symbol(parent.empty() ? "matlab_variable" : "matlab_property", name, join_qualified(parent, name), code, lines[index], line_number, current_parent_index(scopes)));
        }
    }

    while (!scopes.empty()) {
        close_scope(scopes, result.symbols, lines, static_cast<int>(lines.size()));
    }
}

void parse_scilab(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::vector<Scope> scopes;
    static const std::regex function_regex{R"(^\s*function\s+(?:(?:\[[^\]]+\]|[A-Za-z]\w*)\s*=\s*)?([A-Za-z]\w*)\s*(?:\(([^)]*)\))?)"};
    static const std::regex deff_regex{R"(^\s*deff\s*\(\s*['"][^=]*=\s*([A-Za-z]\w*)\s*\(([^)]*)\))"};
    static const std::regex assignment_regex{R"(^\s*([A-Za-z]\w*)\s*=)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const int line_number = static_cast<int>(index + 1);
        const auto raw_trimmed = trim(lines[index].text);
        if (raw_trimmed.empty() || raw_trimmed.rfind("//", 0) == 0) {
            continue;
        }

        const auto code = trim(strip_scilab_comment(lines[index].text));
        if (code.empty()) {
            continue;
        }

        const auto lower = lower_text(code);
        std::smatch match;
        if (lower == "endfunction") {
            close_scope(scopes, result.symbols, lines, line_number);
        } else if (std::regex_search(code, match, function_regex) || std::regex_search(code, match, deff_regex)) {
            const auto name = match[1].str();
            auto symbol = make_symbol("scilab_function", name, name, code, lines[index], line_number);
            if (match.size() > 2) {
                symbol.parameters = parse_parameters(match[2].str());
            }
            result.symbols.push_back(symbol);
            scopes.push_back({"scilab_function", name, name, static_cast<int>(result.symbols.size() - 1), line_number});
        } else if (scopes.empty() && std::regex_search(code, match, assignment_regex)) {
            const auto name = match[1].str();
            result.symbols.push_back(make_symbol("scilab_variable", name, name, code, lines[index], line_number));
        }
    }

    while (!scopes.empty()) {
        close_scope(scopes, result.symbols, lines, static_cast<int>(lines.size()));
    }
}

bool looks_like_objective_c(const std::vector<LineInfo>& lines)
{
    for (const auto& line : lines) {
        const auto text = trim(line.text);
        if (text.rfind("@interface", 0) == 0 || text.rfind("@implementation", 0) == 0 ||
            text.rfind("@protocol", 0) == 0 || text.rfind("#import", 0) == 0 ||
            text.rfind("@property", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string strip_objc_comment(const std::string& line)
{
    const auto position = line.find("//");
    return position == std::string::npos ? line : line.substr(0, position);
}

void parse_objective_c_m(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    static const std::regex import_regex{R"(^\s*#import\s+[<"]([^>"]+)[>"])"};
    static const std::regex type_regex{R"(^\s*@(?:interface|implementation|protocol)\s+([A-Za-z_][A-Za-z0-9_]*))"};
    static const std::regex method_regex{R"(^\s*[-+]\s*\(([^)]+)\)\s*([A-Za-z_][A-Za-z0-9_:]*))"};
    static const std::regex property_regex{R"(^\s*@property\b.*\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)"};
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto code = trim(strip_objc_comment(lines[index].text));
        std::smatch match;
        const int line_number = static_cast<int>(index + 1);
        if (std::regex_search(code, match, import_regex)) {
            result.symbols.push_back(make_symbol("objc_import", match[1].str(), match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, type_regex)) {
            result.symbols.push_back(make_symbol("objc_type", match[1].str(), match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, method_regex)) {
            auto symbol = make_symbol("objc_method", match[2].str(), match[2].str(), code, lines[index], line_number);
            symbol.return_type = trim(match[1].str());
            result.symbols.push_back(symbol);
        } else if (std::regex_search(code, match, property_regex)) {
            result.symbols.push_back(make_symbol("objc_property", match[1].str(), match[1].str(), code, lines[index], line_number));
        }
    }
}

} // namespace

namespace repolens {

std::string MatlabInterpreter::language_id() const
{
    return "matlab";
}

std::vector<std::string> MatlabInterpreter::file_extensions() const
{
    return {".m", ".mlx", ".sci", ".sce", ".tst", ".dem"};
}

ParseResult MatlabInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto extension = extension_for(file);

    if (extension == ".sci" || extension == ".sce" || extension == ".tst" || extension == ".dem") {
        result.language = "scilab";
        parse_scilab(lines, result);
    } else if (extension == ".m" && looks_like_objective_c(lines)) {
        result.language = "objective-c";
        parse_objective_c_m(lines, result);
    } else {
        result.language = "matlab-octave";
        parse_matlab_or_octave(lines, result);
    }

    return result;
}

} // namespace repolens
