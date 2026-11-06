// One fence: a frameless window that shows its entries, and answers the page's requests to change them.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "DirectoryWatch.h"
#include "Fence.h"
#include "WebViewWindow.h"

namespace nofences {

class FenceApp;

class FenceWindow : public WebViewWindow {
public:
    FenceWindow(FenceApp& app, Fence fence);

    std::string html() const override;
    std::string title() const override { return fence_.name; }
    std::string pageReloadedMessage() const override {
        return "The page of a fence stopped unexpectedly, so it has been loaded again.";
    }

protected:
    void onClosed() override;
    void onFrameChanged() override;

private:
    // An icon as the page shows it, with the write stamp of the file it was read from.
    struct CachedIcon {
        std::int64_t stamp = 0;
        std::string url;
    };

    void registerMethods();
    Json state();
    void save();
    void saveFrame();
    void minify();
    void expand();
    void resizeHeight(int height);
    void removeFence();
    std::string iconFor(const std::string& entry);
    // A file in one of the fence's folders was written, renamed or removed, so the page asks for its state again.
    void onFolderChanged();

    FenceApp& app_;
    Fence fence_;
    bool minified_;
    // The height to return to when the fence expands again.
    int expandedHeight_;
    // Set once the fence is removed, so its folder isn't written again while the window closes.
    bool removed_ = false;
    // Set while saving the window's position is failing, so the failure is reported once.
    bool saveFailed_ = false;
    // Icons by entry, so each is read from the system once, and again only when its file has been written since.
    std::map<std::string, CachedIcon> icons_;
    // Reports changes in the folders that hold the fence's files.
    std::unique_ptr<DirectoryWatch> folderWatch_;
};

}  // namespace nofences
