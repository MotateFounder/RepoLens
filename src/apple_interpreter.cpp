#include "repolens/interpreters/apple_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace {

struct LineInfo { std::string text; int char_start = 0; };

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("Failed to read Apple source file: " + path);
    std::vector<LineInfo> lines;
    std::string line;
    int offset = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string extension_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.extension().string());
}

std::string strip_comment(const std::string& line)
{
    const auto position = line.find("//");
    return position == std::string::npos ? line : line.substr(0, position);
}

repolens::CodeSymbol make_symbol(const std::string& kind, const std::string& name, const std::string& signature, const LineInfo& line, int line_number)
{
    repolens::CodeSymbol symbol;
    symbol.kind = kind;
    symbol.name = name;
    symbol.qualified_name = name;
    symbol.signature = trim(signature);
    symbol.line_start = line_number;
    symbol.line_end = line_number;
    const auto position = line.text.find(name);
    symbol.char_start = line.char_start + static_cast<int>(position == std::string::npos ? 0 : position);
    symbol.char_end = line.char_start + static_cast<int>(line.text.size());
    symbol.char_count = std::max(0, symbol.char_end - symbol.char_start);
    return symbol;
}

std::vector<repolens::CodeParameter> split_params(const std::string& text)
{
    std::vector<repolens::CodeParameter> params;
    std::stringstream stream{text};
    std::string part;
    int pos = 0;
    while (std::getline(stream, part, ',')) {
        part = trim(part);
        if (part.empty()) continue;
        repolens::CodeParameter p;
        p.position = pos++;
        const auto colon = part.find(':');
        p.name = trim(colon == std::string::npos ? part : part.substr(0, colon));
        p.type = colon == std::string::npos ? "" : trim(part.substr(colon + 1));
        params.push_back(p);
    }
    return params;
}

void parse_swift(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex type_regex{R"(^\s*(?:public|private|internal|open|final|actor\s+)*\s*(class|struct|enum|protocol|actor|extension)\s+([A-Za-z_][A-Za-z0-9_]*))"};
    const std::regex function_regex{R"(^\s*(?:public|private|internal|open|static|class|override|mutating|async\s+)*func\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:->\s*([^{]+))?)"};
    const std::regex property_regex{R"(^\s*(?:public|private|internal|static|let|var)\s+(?:let|var\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([^={]+))?)"};
    const std::regex import_regex{R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_]*))"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto code = trim(strip_comment(lines[i].text));
        std::smatch match;
        if (std::regex_search(code, match, import_regex)) {
            result.symbols.push_back(make_symbol("swift_import", match[1].str(), code, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(code, match, type_regex)) {
            result.symbols.push_back(make_symbol("swift_" + lower_text(match[1].str()), match[2].str(), code, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(code, match, function_regex)) {
            auto symbol = make_symbol("swift_function", match[1].str(), code, lines[i], static_cast<int>(i + 1));
            symbol.parameters = split_params(match[2].str());
            if (match.size() > 3) symbol.return_type = trim(match[3].str());
            result.symbols.push_back(symbol);
        } else if (std::regex_search(code, match, property_regex)) {
            auto symbol = make_symbol("swift_property", match[1].str(), code, lines[i], static_cast<int>(i + 1));
            if (match.size() > 2) symbol.return_type = trim(match[2].str());
            result.symbols.push_back(symbol);
        }
    }
}

void parse_objc(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex interface_regex{R"(^\s*@(?:interface|implementation|protocol)\s+([A-Za-z_][A-Za-z0-9_]*))"};
    const std::regex method_regex{R"(^\s*[-+]\s*\(([^)]+)\)\s*([A-Za-z_][A-Za-z0-9_:]*))"};
    const std::regex property_regex{R"(^\s*@property\b.*\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)"};
    const std::regex import_regex{R"(^\s*#import\s+[<"]([^>"]+)[>"])"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto code = trim(strip_comment(lines[i].text));
        std::smatch match;
        if (std::regex_search(code, match, import_regex)) {
            result.symbols.push_back(make_symbol("objc_import", match[1].str(), code, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(code, match, interface_regex)) {
            result.symbols.push_back(make_symbol("objc_type", match[1].str(), code, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(code, match, method_regex)) {
            auto symbol = make_symbol("objc_method", match[2].str(), code, lines[i], static_cast<int>(i + 1));
            symbol.return_type = trim(match[1].str());
            result.symbols.push_back(symbol);
        } else if (std::regex_search(code, match, property_regex)) {
            result.symbols.push_back(make_symbol("objc_property", match[1].str(), code, lines[i], static_cast<int>(i + 1)));
        }
    }
}

} // namespace

namespace repolens {

std::string AppleInterpreter::language_id() const { return "apple"; }

std::vector<std::string> AppleInterpreter::file_extensions() const { return {".swift", ".mm", ".h"}; }

ParseResult AppleInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    if (extension_for(file) == ".swift") {
        result.language = "swift";
        parse_swift(lines, result);
    } else {
        result.language = "objective-c";
        parse_objc(lines, result);
    }
    return result;
}

} // namespace repolens
