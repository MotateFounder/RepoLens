#pragma once

#include "repolens/interpreters/language_interpreter.hpp"

namespace repolens {

class RInterpreter final : public ILanguageInterpreter {
public:
    std::string language_id() const override;
    std::vector<std::string> file_extensions() const override;
    ParseResult parse_file(const FileMetadata& file) const override;
};

} // namespace repolens
