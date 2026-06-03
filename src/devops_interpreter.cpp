#include "repolens/interpreters/devops_interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace {

struct LineInfo { std::string text; int char_start = 0; };

std::vector<LineInfo> read_lines(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("Failed to read DevOps file: " + path);
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

std::string filename_for(const repolens::FileMetadata& file)
{
    return lower_text(std::filesystem::path{file.relative_path}.filename().string());
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

void parse_dockerfile(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex instruction_regex{R"(^\s*(FROM|RUN|COPY|ADD|CMD|ENTRYPOINT|ARG|ENV|EXPOSE|WORKDIR)\s+(.+))", std::regex_constants::icase};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch match;
        if (std::regex_search(lines[i].text, match, instruction_regex)) {
            result.symbols.push_back(make_symbol("docker_" + lower_text(match[1].str()), trim(match[2].str()), lines[i].text, lines[i], static_cast<int>(i + 1)));
        }
    }
}

void parse_yaml(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    std::string section;
    const std::regex key_regex{R"(^\s*([A-Za-z0-9_.\-]+):\s*(.*))"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto text = trim(lines[i].text);
        std::smatch match;
        if (std::regex_search(lines[i].text, match, key_regex)) {
            const auto key = match[1].str();
            if (key == "services" || key == "volumes" || key == "networks" || key == "jobs" || key == "steps") {
                section = key;
                result.symbols.push_back(make_symbol("yaml_section", key, text, lines[i], static_cast<int>(i + 1)));
            } else if (!section.empty()) {
                result.symbols.push_back(make_symbol(section == "services" ? "compose_service" : "yaml_property", key, text, lines[i], static_cast<int>(i + 1)));
            } else {
                result.symbols.push_back(make_symbol("yaml_property", key, text, lines[i], static_cast<int>(i + 1)));
            }
        }
    }
}

void parse_terraform(const std::vector<LineInfo>& lines, repolens::ParseResult& result)
{
    const std::regex block_regex{R"REGEX(^\s*(resource|data|module|variable|output|provider|locals)\s*(?:"([^"]+)")?\s*(?:"([^"]+)")?)REGEX"};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch match;
        if (std::regex_search(lines[i].text, match, block_regex)) {
            const auto kind = "terraform_" + lower_text(match[1].str());
            const auto name = match[3].matched ? match[2].str() + "." + match[3].str() : (match[2].matched ? match[2].str() : match[1].str());
            result.symbols.push_back(make_symbol(kind, name, lines[i].text, lines[i], static_cast<int>(i + 1)));
        }
    }
}

} // namespace

namespace repolens {

std::string DevOpsInterpreter::language_id() const { return "devops"; }

std::vector<std::string> DevOpsInterpreter::file_extensions() const
{
    return {".yaml", ".yml", ".tf", ".tfvars", "Dockerfile", "docker-compose.yml", "docker-compose.yaml", "compose.yml", "compose.yaml"};
}

ParseResult DevOpsInterpreter::parse_file(const FileMetadata& file) const
{
    ParseResult result;
    const auto lines = read_lines(file.absolute_path);
    const auto filename = filename_for(file);
    const auto extension = extension_for(file);
    if (filename == "dockerfile") {
        result.language = "dockerfile";
        parse_dockerfile(lines, result);
    } else if (extension == ".tf" || extension == ".tfvars") {
        result.language = "terraform";
        parse_terraform(lines, result);
    } else {
        result.language = "yaml";
        parse_yaml(lines, result);
    }
    return result;
}

} // namespace repolens
