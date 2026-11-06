#include "Entries.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "Platform.h"
#include "Text.h"

namespace nofences {

namespace {

// Names for Steam games that aren't installed here, so there's no manifest to read them from.
const std::map<std::string, std::string> kKnownSteamNames = {
    {"1091500", "Cyberpunk 2077"},
    {"782330", "DOOM Eternal"},
    {"1245620", "ELDEN RING"},
    {"288470", "Fable Anniversary"},
    {"377160", "Fallout 4"},
    {"1593500", "God of War"},
    {"2322010", "God of War Ragnarök"},
    {"414340", "Hellblade: Senua's Sacrifice"},
    {"2461850", "Senua's Saga: Hellblade II"},
    {"990080", "Hogwarts Legacy"},
    {"550", "Left 4 Dead 2"},
    {"241930", "Shadow of Mordor"},
    {"1172380", "STAR WARS Jedi: Fallen Order"},
    {"1332010", "Stray"},
};

// Steam writes backslashes doubled in its library file.
std::string unescapeVdf(const std::string& value) {
    std::string unescaped;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
            ++i;
        }
        unescaped += value[i];
    }
    return unescaped;
}

std::vector<std::filesystem::path> steamLibraryFolders() {
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    for (const auto& root : steamRoots()) {
        const auto steamapps = root / "steamapps";
        if (!std::filesystem::is_directory(steamapps, error)) {
            continue;
        }
        folders.push_back(steamapps);
        const auto library = readFile(steamapps / "libraryfolders.vdf");
        if (!library) {
            continue;
        }
        static const std::regex pathLine(R"re("path"\s+"([^"]+)")re");
        for (std::sregex_iterator match(library->begin(), library->end(), pathLine), end; match != end; ++match) {
            const auto extra = pathFromUtf8(unescapeVdf((*match)[1].str())) / "steamapps";
            if (std::filesystem::is_directory(extra, error)) {
                folders.push_back(extra);
            }
        }
    }
    std::vector<std::filesystem::path> unique;
    std::set<std::filesystem::path> seen;
    for (const auto& folder : folders) {
        if (seen.insert(folder).second) {
            unique.push_back(folder);
        }
    }
    return unique;
}

// The name from a Steam game's manifest, when one of the libraries has it installed.
std::optional<std::string> installedSteamName(const std::string& appId) {
    static const std::regex nameLine(R"re("name"\s+"([^"]+)")re");
    for (const auto& steamapps : steamLibraryFolders()) {
        const auto manifest = readFile(steamapps / ("appmanifest_" + appId + ".acf"));
        std::smatch match;
        if (manifest && std::regex_search(*manifest, match, nameLine)) {
            return match[1].str();
        }
    }
    return std::nullopt;
}

std::string steamAppName(const std::string& appId) {
    static std::map<std::string, std::string> cache;
    auto cached = cache.find(appId);
    if (cached != cache.end()) {
        return cached->second;
    }
    std::string name;
    if (auto installed = installedSteamName(appId)) {
        name = *installed;
    } else if (auto known = kKnownSteamNames.find(appId); known != kKnownSteamNames.end()) {
        name = known->second;
    } else {
        name = "Steam app " + appId;
    }
    cache[appId] = name;
    return name;
}

// The app ID in a steam://rungameid/<id> link, or in any other steam:// address that ends in one.
std::optional<std::string> steamAppIdOf(const std::string& uri) {
    if (!startsWith(uri, "steam://")) {
        return std::nullopt;
    }
    const std::string target = uri.substr(8, uri.find_first_of("?#", 8) - 8);
    return target.substr(target.rfind('/') + 1);
}

// Steam names a game's 32 pixel icon by the SHA-1 of its contents, so the name is 40 hex digits.
bool isSteamIconName(const std::filesystem::path& file) {
    const std::string name = pathToUtf8(file.stem());
    return file.extension() == ".jpg" && name.size() == 40 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// The icon Steam caches for a game. Only a game Steam has shown has one.
std::optional<std::filesystem::path> steamIconFile(const std::string& appId) {
    std::error_code error;
    for (const auto& root : steamRoots()) {
        const auto folder = root / "appcache" / "librarycache" / appId;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error) && isSteamIconName(entry.path())) {
                return entry.path();
            }
        }
    }
    return std::nullopt;
}

// The Icon= value from a .desktop file's [Desktop Entry] section, which names a theme icon or a picture's path.
std::optional<std::string> desktopEntryIcon(const std::string& entry) {
    if (isLink(entry) || pathFromUtf8(entry).extension() != ".desktop") {
        return std::nullopt;
    }
    const auto text = readFile(pathFromUtf8(entry));
    if (!text) {
        return std::nullopt;
    }
    bool inEntry = false;
    std::istringstream lines(*text);
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (!line.empty() && line.front() == '[') {
            inEntry = line == "[Desktop Entry]";
        } else if (inEntry && startsWith(line, "Icon=")) {
            const std::string icon = trim(line.substr(5));
            return icon.empty() ? std::nullopt : std::optional<std::string>(icon);
        }
    }
    return std::nullopt;
}

// The picture that stands for an entry, when it has one: the icon for a Steam link, the file itself for an image, or the
// picture a .desktop file's Icon= points at.
std::optional<std::filesystem::path> pictureFor(const std::string& entry) {
    if (const auto appId = steamAppIdOf(entry)) {
        return steamIconFile(*appId);
    }
    std::error_code error;
    if (!isLink(entry) && isImageFile(entry) && std::filesystem::is_regular_file(pathFromUtf8(entry), error)) {
        return pathFromUtf8(entry);
    }
    if (const auto icon = desktopEntryIcon(entry)) {
        const auto path = pathFromUtf8(*icon);
        if (path.is_absolute() && std::filesystem::is_regular_file(path, error)) {
            return path;
        }
    }
    return std::nullopt;
}

// The game's name for a steam:// link, otherwise the link's site.
std::string linkLabel(const std::string& uri) {
    if (const auto appId = steamAppIdOf(uri)) {
        return steamAppName(*appId);
    }
    const auto schemeEnd = uri.find("://");
    const std::string host = uri.substr(schemeEnd + 3, uri.find_first_of("/?#", schemeEnd + 3) - schemeEnd - 3);
    return host.empty() ? uri : host;
}

}  // namespace

bool isLink(const std::string& entry) {
    return entry.find("://") != std::string::npos;
}

std::optional<std::string> pathFromFileUri(const std::string& uri) {
    if (!startsWith(uri, "file://")) {
        return std::nullopt;
    }
    // Everything from the first slash after the host, which is empty for a local file.
    const std::string rest = uri.substr(7);
    const auto slash = rest.find('/');
    std::string path = percentDecode(slash == std::string::npos ? std::string() : rest.substr(slash));
#ifdef _WIN32
    // A Windows drive is written /C:/..., with the slash before the drive letter.
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(0, 1);
    }
#endif
    return path;
}

bool isImageFile(const std::string& path) {
    static const std::set<std::string> imageSuffixes = {".bmp", ".gif", ".jpg", ".jpeg", ".png", ".tiff", ".tif"};
    std::string suffix = pathToUtf8(pathFromUtf8(path).extension());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return imageSuffixes.count(suffix) > 0;
}

std::string entryLabel(const std::string& entry) {
    if (isLink(entry)) {
        return linkLabel(entry);
    }
    return pathToUtf8(pathFromUtf8(entry).stem());
}

bool entryExists(const std::string& entry) {
    if (isLink(entry)) {
        return true;
    }
    std::error_code error;
    return std::filesystem::exists(pathFromUtf8(entry), error);
}

std::int64_t entryWriteStamp(const std::string& entry) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(pathFromUtf8(entry), error);
    return error ? 0 : static_cast<std::int64_t>(written.time_since_epoch().count());
}

std::optional<std::vector<unsigned char>> entryIconPng(const std::string& entry) {
    if (const auto picture = pictureFor(entry)) {
        if (auto png = pictureIconPng(*picture)) {
            return png;
        }
    }
    if (const auto icon = desktopEntryIcon(entry); icon && !pathFromUtf8(*icon).is_absolute()) {
        if (auto png = themeIconPng(*icon)) {
            return png;
        }
    }
    return systemIconPng(entry);
}

}  // namespace nofences
