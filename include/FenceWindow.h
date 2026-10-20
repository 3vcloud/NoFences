// One fence: a frameless window that shows its entries, and answers the page's requests to change them.

#pragma once

#include <map>
#include <string>

#include "Fence.h"
#include "WebViewWindow.h"

namespace nofences {

class FenceApp;

class FenceWindow : public gwl::WebViewWindow {
public:
    FenceWindow(FenceApp& app, Fence fence);

    std::string html() const override;
    std::string title() const override { return fence_.name; }

protected:
    void onClosed() override;
    void onFrameChanged() override;

private:
    void registerMethods();
    gwl::Json state();
    void save();
    void saveFrame();
    void minify();
    void expand();
    void resizeHeight(int height);
    void removeFence();
    std::string iconFor(const std::string& entry);

    FenceApp& app_;
    Fence fence_;
    bool minified_;
    // The height to return to when the fence expands again.
    int expandedHeight_;
    // Set once the fence is removed, so its folder isn't written again while the window closes.
    bool removed_ = false;
    // Set while saving the window's position is failing, so the failure is reported once.
    bool saveFailed_ = false;
    // Icon data URLs by entry, so each icon is read from the system once.
    std::map<std::string, std::string> icons_;
};

}  // namespace nofences
