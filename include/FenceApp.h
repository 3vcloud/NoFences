// The application: the saved fences, the windows that show them, and the end of the program once the last one closes.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Fence.h"

namespace nofences {

class FenceWindow;

class FenceApp {
public:
    explicit FenceApp(FenceStore store);
    FenceApp(const FenceApp&) = delete;
    FenceApp& operator=(const FenceApp&) = delete;
    ~FenceApp();

    const FenceStore& store() const { return store_; }

    // Opens every saved fence, or a first fence when there are none.
    void start();
    void createFence(const std::string& name);
    // Called by a fence when its window closes. The fence stays saved and opens again next time.
    void windowClosed(FenceWindow* window);

private:
    void openFence(Fence fence);

    FenceStore store_;
    std::vector<std::unique_ptr<FenceWindow>> windows_;
    // Closed windows are kept until the event loop is past their close, since they're closed from inside their own callbacks.
    std::vector<std::unique_ptr<FenceWindow>> closed_;
};

}  // namespace nofences
