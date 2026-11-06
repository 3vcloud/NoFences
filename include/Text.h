// Text helpers shared by the fence files, the entries and the icons.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nofences {

// The file's bytes as text, or nothing when it can't be read.
std::optional<std::string> readFile(const std::filesystem::path& path);

bool startsWith(const std::string& text, const std::string& prefix);

// Decodes %XX escapes, leaving anything else as it is.
std::string percentDecode(const std::string& text);

std::string base64Encode(const std::vector<unsigned char>& bytes);

std::string trim(const std::string& text);

}  // namespace nofences
