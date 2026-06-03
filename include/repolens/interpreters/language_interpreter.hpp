#pragma once

#include "repolens/sqlite_database.hpp"

#include <string>
#include <vector>

namespace repolens {

struct CodeParameter {
    std::string name;
    std::string type;
    std::string default_value;
    int position = 0;
    std::string direction;
};

struct CodeSymbol {
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string return_type;
    std::string visibility;
    std::string modifiers;
    int line_start = 0;
    int line_end = 0;
    int char_start = 0;
    int char_end = 0;
    int char_count = 0;
    int parent_index = -1;
    std::vector<std::string> base_types;
    std::vector<CodeParameter> parameters;
};

struct SymbolRelation {
    std::string source_symbol_name;
    std::string target_symbol_name;
    std::string relation_type;
    std::string source_text;
    std::string target_text;
    double confidence = 1.0;
};

struct ParseResult {
    bool success = true;
    std::string language;
    std::vector<CodeSymbol> symbols;
    std::vector<SymbolRelation> relations;
    std::vector<std::string> diagnostics;
};

class ILanguageInterpreter {
public:
    virtual ~ILanguageInterpreter() = default;

    virtual std::string language_id() const = 0;
    virtual std::vector<std::string> file_extensions() const = 0;
    virtual ParseResult parse_file(const FileMetadata& file) const = 0;
};

} // namespace repolens
