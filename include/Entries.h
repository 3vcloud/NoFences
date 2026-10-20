// What a fence entry is and how it's shown. An entry is a local path, or a link such as steam://rungameid/<id>.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nofences {

bool isLink(const std::string& entry);

// The local path a file:// address names. Nothing when the text isn't one.
std::optional<std::string> pathFromFileUri(const std::string& uri);

bool isImageFile(const std::string& path);

// The name an entry is labelled with: the file name without its extension, or the name of the link's game or site.
std::string entryLabel(const std::string& entry);

// Whether the entry can be shown. A link always can; a path only while it exists.
bool entryExists(const std::string& entry);

// When the entry's file was last written, as a number that changes whenever it's written again. Zero for an entry with no file.
std::int64_t entryWriteStamp(const std::string& entry);

// The entry's icon as PNG bytes, 32 pixels across. A Steam link shows its game's artwork, an image shows itself, and
// everything else shows the system's icon.
std::optional<std::vector<unsigned char>> entryIconPng(const std::string& entry);

}  // namespace nofences
