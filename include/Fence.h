// A fence: its place, size, settings and entries, and the folder each one is saved in.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace nofences {

constexpr int kDefaultTitleHeight = 35;
constexpr int kMinTitleHeight = 16;
constexpr int kMaxTitleHeight = 100;

struct Fence {
    std::string id;
    std::string name;
    int posX = 100;
    int posY = 250;
    int width = 300;
    int height = 300;
    bool locked = false;
    bool canMinify = false;
    int titleHeight = kDefaultTitleHeight;
    // Each entry is a local path or a link such as steam://rungameid/<id>.
    std::vector<std::string> files;
};

// The title height, or the default when the value is outside the range the title can take.
int validTitleHeight(int height);

// A new random version 4 identifier, in the form the Windows app uses for its folders.
std::string newFenceId();

// Keeps each fence in its own folder under the data directory, with its settings in __fence_metadata.xml. That's the
// same layout the Windows app uses, so fences carry over.
class FenceStore {
public:
    explicit FenceStore(std::filesystem::path base);

    // Every saved fence, in folder order. A folder whose file is damaged is left out and named in skipped, with the reason.
    // Throws when the data folder itself can't be read.
    std::vector<Fence> loadAll(std::vector<std::string>& skipped) const;
    // Writes the fence's settings. Throws when the file can't be written.
    void save(const Fence& fence) const;
    // Deletes the fence's folder. Throws when it can't.
    void remove(const Fence& fence) const;
    std::filesystem::path metadataPath(const Fence& fence) const;

private:
    std::filesystem::path base_;
};

}  // namespace nofences
