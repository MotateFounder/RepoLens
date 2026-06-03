#include "repolens/interpreters/shell_interpreter.hpp"

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
        throw std::runtime_error("Failed to read shell file: " + path);
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

repolens::CodeSymbol make_symbol(const std::string& kind, const std::string& name, const std::string& signature, const LineInfo& line, int line_number)
{
    repolens::CodeSymbol symbol;
    symbol.kind = kind;
    symbol.name = name;
    symbol.qualified_name = name;
    symbol.signature = normalize_spaces(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto position = line.text.find(name);
    symbol.char_start = line.char_start + static_cast<int>(position == std::string::npos ? 0 : position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    return symbol;
}

void parse_posix_shell(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex function_regex{R"(^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(\))?\s*\{)"};
    const std::regex export_regex{R"(^\s*export\s+([A-Za-z_][A-Za-z0-9_]*)=?)"};
    const std::regex alias_regex{R"(^\s*alias\s+([A-Za-z_][A-Za-z0-9_\-]*)=)"};
    const std::regex source_regex{R"(^\s*(?:source|\.)\s+(.+))"};
    const std::regex command_regex{R"(^\s*([A-Za-z_][A-Za-z0-9_\-\.]*)\b)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto code = trim(strip_comment(lines[index].text));
        if (code.empty() || code.rfind("#!", 0) == 0) {
            continue;
        }
        const int line_number = static_cast<int>(index + 1);
        std::smatch match;
        if (std::regex_search(code, match, function_regex)) {
            result.symbols.push_back(make_symbol("shell_function", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, export_regex)) {
            result.symbols.push_back(make_symbol("shell_export", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, alias_regex)) {
            result.symbols.push_back(make_symbol("shell_alias", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, source_regex)) {
            result.symbols.push_back(make_symbol("shell_source", trim(match[1].str()), code, lines[index], line_number));
        } else if (std::regex_search(code, match, command_regex)) {
            const auto name = match[1].str();
            if (name != "if" && name != "then" && name != "fi" && name != "for" && name != "do" && name != "done" && name != "case" && name != "esac") {
                result.symbols.push_back(make_symbol("shell_command", name, code, lines[index], line_number));
            }
        }
    }
}

void parse_powershell(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex function_regex{R"(^\s*function\s+([A-Za-z_][A-Za-z0-9_\-]*)\b)"};
    const std::regex param_regex{R"(^\s*param\s*\()"};
    const std::regex variable_regex{R"(^\s*(?:\[[^\]]+\]\s*)?\$([A-Za-z_][A-Za-z0-9_]*)\s*=)"};
    const std::regex alias_regex{R"(^\s*(?:Set-Alias|New-Alias)\s+(?:-Name\s+)?([A-Za-z_][A-Za-z0-9_\-]*))"};
    const std::regex manifest_regex{R"(^\s*(RootModule|ModuleVersion|GUID|Author|FunctionsToExport|CmdletsToExport)\s*=)"};
    const std::regex command_regex{R"(^\s*([A-Za-z_][A-Za-z0-9_]*-[A-Za-z_][A-Za-z0-9_]*)\b)"};

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto code = trim(strip_comment(lines[index].text));
        if (code.empty()) {
            continue;
        }
        const int line_number = static_cast<int>(index + 1);
        std::smatch match;
        if (std::regex_search(code, match, function_regex)) {
            result.symbols.push_back(make_symbol("powershell_function", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, param_regex)) {
            result.symbols.push_back(make_symbol("powershell_param_block", "param", code, lines[index], line_number));
        } else if (std::regex_search(code, match, variable_regex)) {
            result.symbols.push_back(make_symbol("powershell_variable", "$" + match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, alias_regex)) {
            result.symbols.push_back(make_symbol("powershell_alias", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, manifest_regex)) {
            result.symbols.push_back(make_symbol("powershell_manifest_property", match[1].str(), code, lines[index], line_number));
        } else if (std::regex_search(code, match, command_regex)) {
            result.symbols.push_back(make_symbol("powershell_command", match[1].str(), code, lines[index], line_number));
        }
    }
}

} // namespace

namespace repolens {

std::string ShellInterpreter::language_id() const
{
    return "shell";
}

std::vector<std::string> ShellInterpreter::file_extensions() const
{
    return {".sh", ".bash", ".zsh", ".ps1", ".psm1", ".psd1"};
}

ParseResult ShellInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto extension = extension_for(file);
    if (extension == ".ps1" || extension == ".psm1" || extension == ".psd1") {
        result.language = "powershell";
        parse_powershell(lines, result);
    } else {
        result.language = "shell";
        parse_posix_shell(lines, result);
    }
    return result;
}

} // namespace repolens
