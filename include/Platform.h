// The desktop facts the fences need. Each platform defines them in platform/<os>/Desktop.cpp.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nofences {

// Where fences are kept when NOFENCES_DATA_DIR doesn't say otherwise.
std::filesystem::path defaultDataDirectory();

// The user's desktop folder, which the fence menu opens.
std::filesystem::path desktopDirectory();

// Opens a file, a folder or a link with the program the desktop uses for it. Doesn't wait for that program to close. Throws
// when the program can't be started; a failure after that is reported to the user as it happens.
void openTarget(const std::string& target);

// The folders Steam keeps its game manifests in: the default install and any others it knows about.
std::vector<std::filesystem::path> steamRoots();

// A picture fitted into a 32 pixel square as PNG bytes, keeping its shape and transparent around the edges.
std::optional<std::vector<unsigned char>> pictureIconPng(const std::filesystem::path& picture);

// The system's icon for a file, folder or link, as 32 pixel PNG bytes.
std::optional<std::vector<unsigned char>> systemIconPng(const std::string& entry);

// The icon the desktop's theme has under a name, as 32 pixel PNG bytes. Platforms without themes have none.
std::optional<std::vector<unsigned char>> themeIconPng(const std::string& name);

// Returns false when another NoFences already holds the data folder. The lock lasts until the program exits.
bool claimSingleInstance(const std::filesystem::path& dataDirectory);

// Fences keep their text as UTF-8, so paths are converted to and from it.
inline std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace nofences
