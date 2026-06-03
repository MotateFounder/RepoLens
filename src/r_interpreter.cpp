#include "repolens/interpreters/r_interpreter.hpp"

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
    if (!file) throw std::runtime_error("Failed to read R file: " + path);
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
        const auto eq = part.find('=');
        p.name = trim(eq == std::string::npos ? part : part.substr(0, eq));
        if (eq != std::string::npos) p.default_value = trim(part.substr(eq + 1));
        params.push_back(p);
    }
    return params;
}

} // namespace

namespace repolens {

std::string RInterpreter::language_id() const { return "r"; }

std::vector<std::string> RInterpreter::file_extensions() const { return {".r", ".R", ".Rmd", ".rmd"}; }

ParseResult RInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    result.language = extension_for(file) == ".rmd" ? "rmarkdown" : "r";
    const auto lines = read_lines(file.absolute_path);
    const std::regex function_regex{R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:<-|=)\s*function\s*\(([^)]*)\))"};
    const std::regex assignment_regex{R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:<-|=))"};
    const std::regex package_regex{R"(\b(?:library|require)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))"};
    const std::regex chunk_regex{R"(^```\{r\s*([^,}]*)?)"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto text = trim(lines[i].text);
        if (text.empty() || text.front() == '#') continue;
        std::smatch match;
        if (std::regex_search(text, match, chunk_regex)) {
            const auto name = trim(match[1].str()).empty() ? "r_chunk" : trim(match[1].str());
            result.symbols.push_back(make_symbol("rmarkdown_chunk", name, text, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(text, match, function_regex)) {
            auto symbol = make_symbol("r_function", match[1].str(), text, lines[i], static_cast<int>(i + 1));
            symbol.parameters = split_params(match[2].str());
            result.symbols.push_back(symbol);
        } else if (std::regex_search(text, match, package_regex)) {
            result.symbols.push_back(make_symbol("r_package", match[1].str(), text, lines[i], static_cast<int>(i + 1)));
        } else if (std::regex_search(text, match, assignment_regex)) {
            result.symbols.push_back(make_symbol("r_variable", match[1].str(), text, lines[i], static_cast<int>(i + 1)));
        }
    }
    return result;
}

} // namespace repolens
