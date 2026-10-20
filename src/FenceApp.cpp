#include "FenceApp.h"

#include <algorithm>
#include <utility>

#include "FenceWindow.h"
#include "WebViewWindow.h"

namespace nofences {

FenceApp::FenceApp(FenceStore store) : store_(std::move(store)) {}

FenceApp::~FenceApp() = default;

void FenceApp::start() {
    std::vector<std::string> skipped;
    std::vector<Fence> fences = store_.loadAll(skipped);
    std::vector<std::string> failures;
    for (Fence& fence : fences) {
        const std::string name = fence.name;
        try {
            openFence(std::move(fence));
        } catch (const gwl::RuntimeMissingError&) {
            throw;
        } catch (const std::exception& problem) {
            failures.push_back(name + " (" + problem.what() + ")");
        }
    }
    if (fences.empty() && skipped.empty()) {
        createFence("First fence");
    }
    // The fences that did open keep running; the ones that didn't are named, so the user knows what's missing.
    if (!skipped.empty() || !failures.empty()) {
        std::string message;
        if (!skipped.empty()) {
            message += "These fences couldn't be read, so they aren't shown:";
            for (const std::string& item : skipped) {
                message += "\n- " + item;
            }
        }
        if (!failures.empty()) {
            message += std::string(message.empty() ? "" : "\n\n") + "These fences couldn't be opened:";
            for (const std::string& item : failures) {
                message += "\n- " + item;
            }
        }
        gwl::showError(message);
    }
}

void FenceApp::createFence(const std::string& name) {
    Fence fence;
    fence.id = newFenceId();
    fence.name = name;
    store_.save(fence);
    openFence(std::move(fence));
}

void FenceApp::openFence(Fence fence) {
    const int x = fence.posX;
    const int y = fence.posY;
    auto window = std::make_unique<FenceWindow>(*this, std::move(fence));
    window->open();
    window->move(x, y);
    windows_.push_back(std::move(window));
}

void FenceApp::windowClosed(FenceWindow* window) {
    auto found = std::find_if(windows_.begin(), windows_.end(), [window](const auto& open) { return open.get() == window; });
    if (found == windows_.end()) {
        return;
    }
    closed_.push_back(std::move(*found));
    windows_.erase(found);
    gwl::runOnUiThread([this] {
        closed_.clear();
        if (windows_.empty()) {
            gwl::stopEventLoop();
        }
    });
}

}  // namespace nofences
