#pragma once

#include "repolens/interpreters/language_interpreter.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace repolens {

class InterpreterRegistry {
public:
    void register_interpreter(std::unique_ptr<ILanguageInterpreter> interpreter);
    const ILanguageInterpreter* find_by_extension(const std::string& extension) const;
    const ILanguageInterpreter* find_for_file(const FileMetadata& file) const;

private:
    std::vector<std::unique_ptr<ILanguageInterpreter>> interpreters_;
    std::unordered_map<std::string, ILanguageInterpreter*> interpreters_by_extension_;
    std::unordered_map<std::string, ILanguageInterpreter*> interpreters_by_filename_;
};

} // namespace repolens
