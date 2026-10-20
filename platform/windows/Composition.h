// DirectComposition for a see-through window. The web view draws into a visual of the window's own tree, so the pixels the page
// leaves transparent show whatever is behind the window. Drags over the window are passed to the web view too.

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <WebView2.h>

namespace gwl {
namespace composition {

// The DirectComposition tree that a window's web view is attached to.
class Surface {
public:
    Surface() = default;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Builds the tree for hwnd. Throws when Direct3D or DirectComposition can't be started.
    void create(HWND hwnd);
    // The visual the web view draws into.
    IUnknown* rootVisual() const { return visual_; }
    void commit();

private:
    ID3D11Device* direct3d_ = nullptr;
    IDCompositionDevice* device_ = nullptr;
    IDCompositionTarget* target_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
};

// Passes drags over hwnd to the web view. Undone by revokeDropTarget before the window is destroyed.
void registerDropTarget(HWND hwnd, ICoreWebView2CompositionController* controller);
void revokeDropTarget(HWND hwnd);

}  // namespace composition
}  // namespace gwl
