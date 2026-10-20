#include "Composition.h"

#include <ole2.h>

#include <stdexcept>

namespace gwl {
namespace composition {

namespace {

template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// Converts a drag's screen point to the window's client area, where the web view takes it.
POINT toClient(HWND hwnd, POINTL screen) {
    POINT point{screen.x, screen.y};
    ScreenToClient(hwnd, &point);
    return point;
}

// The drop target the web view's drags go through. Each call is passed on to the composition controller.
class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, ICoreWebView2CompositionController3* controller) : hwnd_(hwnd), controller_(controller) {
        controller_->AddRef();
    }
    ~DropTarget() { release(controller_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (left == 0) {
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keyState, POINTL point, DWORD* effect) override {
        return controller_->DragEnter(data, keyState, toClient(hwnd_, point), effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override {
        return controller_->DragOver(keyState, toClient(hwnd_, point), effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return controller_->DragLeave(); }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keyState, POINTL point, DWORD* effect) override {
        return controller_->Drop(data, keyState, toClient(hwnd_, point), effect);
    }

private:
    HWND hwnd_;
    ICoreWebView2CompositionController3* controller_;
    ULONG refs_ = 1;
};

}  // namespace

Surface::~Surface() {
    release(visual_);
    release(target_);
    release(device_);
    release(direct3d_);
}

void Surface::create(HWND hwnd) {
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &direct3d_, nullptr, nullptr);
    if (SUCCEEDED(result)) {
        IDXGIDevice* dxgi = nullptr;
        result = direct3d_->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(result)) {
            result = DCompositionCreateDevice(dxgi, IID_PPV_ARGS(&device_));
            dxgi->Release();
        }
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateTargetForHwnd(hwnd, TRUE, &target_);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateVisual(&visual_);
    }
    if (SUCCEEDED(result)) {
        result = target_->SetRoot(visual_);
    }
    if (FAILED(result)) {
        release(visual_);
        release(target_);
        release(device_);
        release(direct3d_);
        throw std::runtime_error("The see-through page couldn't be drawn, since DirectComposition isn't available.");
    }
    commit();
}

void Surface::commit() {
    if (device_) {
        device_->Commit();
    }
}

void registerDropTarget(HWND hwnd, ICoreWebView2CompositionController* controller) {
    ICoreWebView2CompositionController3* controller3 = nullptr;
    if (FAILED(controller->QueryInterface(IID_ICoreWebView2CompositionController3, reinterpret_cast<void**>(&controller3)))) {
        // An older WebView2 runtime can't take drags. The window still works, without drops.
        return;
    }
    // The target keeps its own reference to the controller, so this one is released straight away.
    auto* target = new DropTarget(hwnd, controller3);
    controller3->Release();
    RegisterDragDrop(hwnd, target);
    target->Release();
}

void revokeDropTarget(HWND hwnd) {
    RevokeDragDrop(hwnd);
}

}  // namespace composition
}  // namespace gwl
