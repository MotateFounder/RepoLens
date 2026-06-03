#include "repolens/interpreters/interpreter_registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace {

std::string normalize_extension(std::string extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (!extension.empty() && extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }

    return extension;
}

std::string normalize_filename(std::string filename)
{
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return filename;
}

} // namespace

namespace repolens {

void InterpreterRegistry::register_interpreter(std::unique_ptr<ILanguageInterpreter> interpreter)
{
    if (!interpreter) {
        throw std::runtime_error("Cannot register a null interpreter.");
    }

    const auto extensions = interpreter->file_extensions();
    if (extensions.empty()) {
        throw std::runtime_error("Interpreter must support at least one file extension.");
    }

    ILanguageInterpreter* raw_interpreter = interpreter.get();
    interpreters_.push_back(std::move(interpreter));

    for (const auto& extension : extensions) {
        if (!extension.empty() && extension.front() == '.') {
            interpreters_by_extension_[normalize_extension(extension)] = raw_interpreter;
        } else {
            interpreters_by_extension_[normalize_extension(extension)] = raw_interpreter;
            interpreters_by_filename_[normalize_filename(extension)] = raw_interpreter;
        }
    }
}

const ILanguageInterpreter* InterpreterRegistry::find_by_extension(const std::string& extension) const
{
    const auto found = interpreters_by_extension_.find(normalize_extension(extension));
    if (found == interpreters_by_extension_.end()) {
        return nullptr;
    }

    return found->second;
}

const ILanguageInterpreter* InterpreterRegistry::find_for_file(const FileMetadata& file) const
{
    const auto filename = normalize_filename(std::filesystem::path{file.relative_path}.filename().string());
    const auto found_name = interpreters_by_filename_.find(filename);
    if (found_name != interpreters_by_filename_.end()) {
        return found_name->second;
    }

    return find_by_extension(file.extension);
}

} // namespace repolens
