#include "Text.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace nofences {

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string percentDecode(const std::string& text) {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            decoded += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += text[i];
        }
    }
    return decoded;
}

std::string base64Encode(const std::vector<unsigned char>& bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t first = bytes[i];
        const std::uint32_t second = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t third = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t chunk = (first << 16) | (second << 8) | third;
        encoded += alphabet[(chunk >> 18) & 63];
        encoded += alphabet[(chunk >> 12) & 63];
        encoded += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        encoded += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return encoded;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace nofences
