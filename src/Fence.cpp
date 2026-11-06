#include "Fence.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>

#include "Platform.h"
#include "Text.h"
#include "Xml.h"

namespace nofences {

namespace {

const char* const kMetadataFile = "__fence_metadata.xml";

std::string textOf(const xml::Element& root, const char* name, const std::string& fallback) {
    const xml::Element* child = root.child(name);
    return child ? child->text : fallback;
}

int numberOf(const xml::Element& root, const char* name, int fallback) {
    const xml::Element* child = root.child(name);
    return child ? std::stoi(child->text) : fallback;
}

bool flagOf(const xml::Element& root, const char* name) {
    const xml::Element* child = root.child(name);
    return child && child->text == "true";
}

Fence fenceFromXml(const xml::Element& root) {
    Fence fence;
    fence.id = textOf(root, "Id", "");
    fence.name = textOf(root, "Name", "");
    fence.posX = numberOf(root, "PosX", fence.posX);
    fence.posY = numberOf(root, "PosY", fence.posY);
    fence.width = numberOf(root, "Width", fence.width);
    fence.height = numberOf(root, "Height", fence.height);
    fence.locked = flagOf(root, "Locked");
    fence.canMinify = flagOf(root, "CanMinify");
    fence.titleHeight = validTitleHeight(numberOf(root, "TitleHeight", kDefaultTitleHeight));
    if (const xml::Element* files = root.child("Files")) {
        for (const xml::Element& entry : files->children) {
            if (entry.name == "string" && !entry.text.empty()) {
                fence.files.push_back(entry.text);
            }
        }
    }
    return fence;
}

xml::Element leaf(const std::string& name, const std::string& text) {
    xml::Element element;
    element.name = name;
    element.text = text;
    return element;
}

xml::Element fenceToXml(const Fence& fence) {
    xml::Element root;
    root.name = "FenceInfo";
    root.children = {
        leaf("Id", fence.id),
        leaf("Name", fence.name),
        leaf("PosX", std::to_string(fence.posX)),
        leaf("PosY", std::to_string(fence.posY)),
        leaf("Width", std::to_string(fence.width)),
        leaf("Height", std::to_string(fence.height)),
        leaf("Locked", fence.locked ? "true" : "false"),
        leaf("CanMinify", fence.canMinify ? "true" : "false"),
        leaf("TitleHeight", std::to_string(fence.titleHeight)),
    };
    xml::Element files;
    files.name = "Files";
    for (const std::string& entry : fence.files) {
        files.children.push_back(leaf("string", entry));
    }
    root.children.push_back(files);
    return root;
}

}  // namespace

int validTitleHeight(int height) {
    return height >= kMinTitleHeight && height <= kMaxTitleHeight ? height : kDefaultTitleHeight;
}

std::string newFenceId() {
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) | device());
    unsigned char bytes[16];
    for (unsigned char& byte : bytes) {
        byte = static_cast<unsigned char>(engine() & 0xFF);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    static const char digits[] = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            id += '-';
        }
        id += digits[bytes[i] >> 4];
        id += digits[bytes[i] & 0x0F];
    }
    return id;
}

FenceStore::FenceStore(std::filesystem::path base) : base_(std::move(base)) {}

std::vector<Fence> FenceStore::loadAll(std::vector<std::string>& skipped) const {
    std::vector<Fence> fences;
    std::error_code error;
    std::filesystem::directory_iterator folders(base_, error);
    if (error) {
        throw std::runtime_error("The fence folder " + pathToUtf8(base_) + " can't be read: " + error.message());
    }
    for (const auto& folder : folders) {
        std::error_code entryError;
        const auto metadata = folder.path() / kMetadataFile;
        if (!folder.is_directory(entryError) || !std::filesystem::is_regular_file(metadata, entryError)) {
            continue;
        }
        try {
            const auto text = readFile(metadata);
            if (!text) {
                throw std::runtime_error("the file can't be read");
            }
            Fence fence = fenceFromXml(xml::parse(*text));
            fence.id = pathToUtf8(folder.path().filename());
            fences.push_back(std::move(fence));
        } catch (const std::exception& problem) {
            skipped.push_back(pathToUtf8(folder.path().filename()) + " (" + problem.what() + ")");
        }
    }
    std::sort(fences.begin(), fences.end(), [](const Fence& a, const Fence& b) { return a.id < b.id; });
    return fences;
}

void FenceStore::save(const Fence& fence) const {
    const auto folder = base_ / pathFromUtf8(fence.id);
    std::filesystem::create_directories(folder);
    const auto target = folder / kMetadataFile;
    // Written beside the old file and then renamed over it, so a write that fails never leaves a half-written file.
    const auto staged = folder / (std::string(kMetadataFile) + ".tmp");
    {
        std::ofstream file(staged, std::ios::binary | std::ios::trunc);
        file << xml::write(fenceToXml(fence));
        if (!file) {
            throw std::runtime_error("The fence couldn't be saved to " + pathToUtf8(target));
        }
    }
    std::filesystem::rename(staged, target);
}

void FenceStore::remove(const Fence& fence) const {
    std::error_code error;
    std::filesystem::remove_all(base_ / pathFromUtf8(fence.id), error);
    if (error) {
        throw std::runtime_error("The fence's folder couldn't be deleted: " + error.message());
    }
}

std::filesystem::path FenceStore::metadataPath(const Fence& fence) const {
    return base_ / pathFromUtf8(fence.id) / kMetadataFile;
}

}  // namespace nofences
