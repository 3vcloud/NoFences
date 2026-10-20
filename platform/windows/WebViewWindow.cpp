// Windows: a Win32 window with a WebView2 control. WebView2Loader.dll is loaded with LoadLibrary when the first window is
// made, not through the import table, so the program starts without it. The page's messages come from WebView2's
// message event; the page reads the host through window.chrome.webview.

#include <windows.h>
#include <objbase.h>
#include <windowsx.h>

#include <WebView2.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "WebViewWindow.h"
#include "WebView2.h"
#include "Composition.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace gwl {

namespace {

using CreateEnvironmentFn = HRESULT(STDMETHODCALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

const wchar_t* const kWindowClass = L"NoFencesWebViewWindow";
// Where the WebView2 runtime is downloaded from.
const char* const kWebView2Download = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string toUtf8(const wchar_t* text) {
    if (!text) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr) - 1;
    std::string utf8(length > 0 ? length : 0, '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);
    }
    return utf8;
}

template <typename F>
F loaderFunction(const char* name) {
    static HMODULE loader = LoadLibraryW((executableFolder() / "WebView2Loader.dll").c_str());
    if (!loader) {
        throw std::runtime_error("WebView2Loader.dll couldn't be loaded. Install the WebView2 runtime.");
    }
    FARPROC address = GetProcAddress(loader, name);
    if (!address) {
        throw std::runtime_error(std::string("WebView2Loader.dll is missing ") + name + ".");
    }
    return reinterpret_cast<F>(address);
}

}  // namespace

struct NativeWindow {
    WebViewWindow* owner = nullptr;
    HWND hwnd = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webview = nullptr;
    EventRegistrationToken messageToken{};
    EventRegistrationToken processFailedToken{};
    // Scripts asked for before the web view exists. They run once it's ready.
    std::vector<std::wstring> pendingScripts;
    // Set while the window is being destroyed by its owner, so no close notification is sent for it.
    bool destroying = false;
    // A see-through window draws its page through composition, so its input and cursor go through the composition controller.
    bool transparent = false;
    bool mouseTracking = false;
    ICoreWebView2CompositionController* compositionController = nullptr;
    composition::Surface surface;
    HCURSOR cursor = nullptr;
    // Set while the user drags or resizes the window, so the edges snap to the grid. The window's rect and the cursor's point
    // when the drag began are what the snapping works from.
    bool snapping = false;
    RECT dragStart{};
    POINT dragCursor{};
    // A see-through window's move follows the cursor from the press itself, not through the system's move loop, which starts
    // only after the page has asked for it.
    bool manualMove = false;
    RECT pressRect{};
    POINT pressCursor{};
    // The smallest size the user can resize to, in pixels. Zero means no minimum.
    int minWidth = 0;
    int minHeight = 0;
};

namespace {

void resizeWebView(NativeWindow* native) {
    if (!native->controller || !native->hwnd) {
        return;
    }
    RECT bounds{};
    GetClientRect(native->hwnd, &bounds);
    native->controller->put_Bounds(bounds);
}

void syncWebViewVisibility(NativeWindow* native) {
    if (native->controller && native->hwnd) {
        native->controller->put_IsVisible(IsWindowVisible(native->hwnd) && !IsIconic(native->hwnd));
    }
}

// The page paints its background in the colour WebView2 is given. Alpha 0 leaves it see-through.
void makeBackgroundTransparent(ICoreWebView2Controller* controller) {
    ICoreWebView2Controller2* controller2 = nullptr;
    if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2, reinterpret_cast<void**>(&controller2)))) {
        controller2->put_DefaultBackgroundColor({0, 0, 0, 0});
        controller2->Release();
    }
}

void sendPendingScripts(NativeWindow* native) {
    for (const std::wstring& script : native->pendingScripts) {
        native->webview->ExecuteScript(script.c_str(), webview2::scriptDone());
    }
    native->pendingScripts.clear();
}

// WebView2 runs the page in processes of its own, which can die. A page's process is started again with the page, since the
// page asks the host for its state as it loads. Any other failure leaves the web view unusable, so the window is closed.
// Either way the user is told why.
void recoverFromProcessFailure(NativeWindow* native, COREWEBVIEW2_PROCESS_FAILED_KIND kind) {
    switch (kind) {
        case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
            // A busy page isn't a failure, and it may carry on by itself.
            return;
        case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED: {
            const std::string url = native->owner->devUrl();
            if (!url.empty()) {
                native->webview->Navigate(toWide(url).c_str());
            } else {
                native->webview->NavigateToString(toWide(native->owner->html()).c_str());
            }
            runOnUiThread([] { showError("The page of a fence stopped unexpectedly, so it has been loaded again."); });
            return;
        }
        default:
            runOnUiThread([] { showError("The web view stopped working, so the window has been closed."); });
            native->owner->close();
    }
}

void attachWebView(NativeWindow* native, ICoreWebView2Controller* controller) {
    native->controller = controller;
    native->controller->AddRef();
    if (native->transparent) {
        makeBackgroundTransparent(controller);
    }
    native->controller->get_CoreWebView2(&native->webview);
    resizeWebView(native);
    syncWebViewVisibility(native);

    native->webview->add_WebMessageReceived(
        webview2::messageReceived([native](ICoreWebView2WebMessageReceivedEventArgs* args) {
            LPWSTR message = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
                native->owner->receive(toUtf8(message));
                CoTaskMemFree(message);
            }
        }),
        &native->messageToken);
    native->webview->add_ProcessFailed(
        webview2::processFailed([native](ICoreWebView2ProcessFailedEventArgs* args) {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            args->get_ProcessFailedKind(&kind);
            recoverFromProcessFailure(native, kind);
        }),
        &native->processFailedToken);
    sendPendingScripts(native);
}

// A see-through window's controller is the composition controller's. Its page draws into the window's visual, and its cursor
// and drags are passed on from the window.
void attachCompositionWebView(NativeWindow* native, ICoreWebView2CompositionController* compositionController,
                              ICoreWebView2Controller* controller) {
    native->compositionController = compositionController;
    native->compositionController->AddRef();
    native->compositionController->put_RootVisualTarget(native->surface.rootVisual());
    native->surface.commit();
    attachWebView(native, controller);

    EventRegistrationToken cursorToken{};
    native->compositionController->add_CursorChanged(
        webview2::cursorChanged([native](ICoreWebView2CompositionController* sender) { sender->get_Cursor(&native->cursor); }),
        &cursorToken);
    composition::registerDropTarget(native->hwnd, native->compositionController);
}

// A drag or resize by the user snaps the edges it moves to this grid, in screen pixels.
const int kSnapGrid = 8;

int snapToGrid(int value) {
    return static_cast<int>(std::lround(static_cast<double>(value) / kSnapGrid)) * kSnapGrid;
}

// Snaps the top-left corner of a window that is being moved, keeping its size. The corner is worked out from where the drag
// started and how far the cursor has gone since, not from the last snapped rect: Windows carries that rect into the next
// step, so snapping it again would round small movements away.
void snapPosition(RECT* rect, const RECT& start, const POINT& cursorDelta) {
    const int width = rect->right - rect->left;
    const int height = rect->bottom - rect->top;
    rect->left = snapToGrid(start.left + cursorDelta.x);
    rect->top = snapToGrid(start.top + cursorDelta.y);
    rect->right = rect->left + width;
    rect->bottom = rect->top + height;
}

// Snaps only the edges that the resize moves, from the same drag start as snapPosition.
void snapSize(RECT* rect, const RECT& start, const POINT& cursorDelta, WPARAM edge) {
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        rect->left = snapToGrid(start.left + cursorDelta.x);
    }
    if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) {
        rect->right = snapToGrid(start.right + cursorDelta.x);
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        rect->top = snapToGrid(start.top + cursorDelta.y);
    }
    if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
        rect->bottom = snapToGrid(start.bottom + cursorDelta.y);
    }
}

// How far the cursor has gone since the move or resize began.
POINT cursorSince(const POINT& start) {
    POINT now{};
    GetCursorPos(&now);
    return {now.x - start.x, now.y - start.y};
}

// The work area (the screen less the taskbar) of the monitor that a rect is over.
RECT workAreaFor(const RECT& rect) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return monitor.rcWork;
}

// Keeps a window that is being moved inside the work area. A window larger than the area is held at its top-left corner.
void clampToWorkArea(RECT* rect) {
    const RECT area = workAreaFor(*rect);
    const int width = rect->right - rect->left;
    const int height = rect->bottom - rect->top;
    rect->left = std::max(area.left, std::min(rect->left, area.right - width));
    rect->top = std::max(area.top, std::min(rect->top, area.bottom - height));
    rect->right = rect->left + width;
    rect->bottom = rect->top + height;
}

// Keeps a window that is being resized at least its minimum size. The edge being dragged gives way, so the opposite edge stays put.
void enforceMinimumSize(RECT* rect, WPARAM edge, int minWidth, int minHeight) {
    if (rect->right - rect->left < minWidth) {
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
            rect->left = rect->right - minWidth;
        } else {
            rect->right = rect->left + minWidth;
        }
    }
    if (rect->bottom - rect->top < minHeight) {
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
            rect->top = rect->bottom - minHeight;
        } else {
            rect->bottom = rect->top + minHeight;
        }
    }
}

// Keeps the edges that a resize moves inside the work area. An edge the resize leaves alone is left as it is, even if it's already past the area.
void clampEdgesToWorkArea(RECT* rect, WPARAM edge) {
    const RECT area = workAreaFor(*rect);
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        rect->left = std::max(rect->left, static_cast<LONG>(area.left));
    }
    if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) {
        rect->right = std::min(rect->right, static_cast<LONG>(area.right));
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        rect->top = std::max(rect->top, static_cast<LONG>(area.top));
    }
    if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
        rect->bottom = std::min(rect->bottom, static_cast<LONG>(area.bottom));
    }
}

// Moves a see-through window with the cursor, from the window's rect and the cursor's point at the press.
void moveWithCursor(NativeWindow* native) {
    RECT target = native->pressRect;
    snapPosition(&target, native->pressRect, cursorSince(native->pressCursor));
    clampToWorkArea(&target);
    SetWindowPos(native->hwnd, nullptr, target.left, target.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The composition controller doesn't read the window's mouse messages, so they're passed to it. Its virtual keys are the
// same bits as the messages' own key flags.
void forwardMouseInput(NativeWindow* native, UINT message, WPARAM wParam, LPARAM lParam) {
    HWND hwnd = native->hwnd;
    COREWEBVIEW2_MOUSE_EVENT_KIND kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
    UINT32 data = 0;
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    switch (message) {
        case WM_MOUSEMOVE:
            if (!native->mouseTracking) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tracking);
                native->mouseTracking = true;
            }
            if (native->manualMove) {
                if (wParam & MK_LBUTTON) {
                    moveWithCursor(native);
                } else {
                    native->manualMove = false;
                }
            }
            break;
        case WM_MOUSELEAVE:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
            native->mouseTracking = false;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            break;
        case WM_LBUTTONDOWN:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
            SetCapture(hwnd);
            GetWindowRect(hwnd, &native->pressRect);
            GetCursorPos(&native->pressCursor);
            native->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            break;
        case WM_LBUTTONUP:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
            native->manualMove = false;
            ReleaseCapture();
            break;
        case WM_RBUTTONDOWN:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
            break;
        case WM_RBUTTONUP:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
            break;
        case WM_MBUTTONDOWN:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
            break;
        case WM_MBUTTONUP:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
            break;
        case WM_XBUTTONDOWN:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
            data = GET_XBUTTON_WPARAM(wParam);
            break;
        case WM_XBUTTONUP:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
            data = GET_XBUTTON_WPARAM(wParam);
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // The wheel reports its point on the screen, where the web view wants it in the window.
            kind = message == WM_MOUSEWHEEL ? COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL : COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
            data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wParam));
            ScreenToClient(hwnd, &point);
            break;
        default:
            break;
    }
    native->compositionController->SendMouseInput(kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(LOWORD(wParam)), data,
                                                  point);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* created = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return 0;
    }
    // Every window is frameless. These come before WM_CREATE too, so they don't wait for the window's NativeWindow.
    switch (message) {
        case WM_NCCALCSIZE:
            // The client area is the whole window: a resizable window keeps its sizing frame for the size loop, but it takes no room.
            if (wParam) {
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            // Keeps Windows from drawing the hidden frame over the page when the window gains or loses the focus.
            return DefWindowProcW(hwnd, message, wParam, -1);
        case WM_GETMINMAXINFO: {
            // A frameless window would maximise over the whole monitor, taskbar included, so it's kept to the work area.
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
                info->ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
            }
            // The window's minimum size, which the system enforces while the user resizes it.
            if (auto* minimum = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); minimum) {
                info->ptMinTrackSize = {minimum->minWidth, minimum->minHeight};
            }
            return 0;
        }
        default:
            break;
    }
    auto* native = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!native) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
        case WM_SIZE:
            resizeWebView(native);
            syncWebViewVisibility(native);
            native->owner->notifyFrameChanged();
            return 0;
        case WM_MOVE:
            native->owner->notifyFrameChanged();
            return 0;
        case WM_ENTERSIZEMOVE:
            GetWindowRect(hwnd, &native->dragStart);
            GetCursorPos(&native->dragCursor);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_MOVING:
            if (native->snapping) {
                auto* rect = reinterpret_cast<RECT*>(lParam);
                snapPosition(rect, native->dragStart, cursorSince(native->dragCursor));
                clampToWorkArea(rect);
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_SIZING:
            if (native->snapping) {
                auto* rect = reinterpret_cast<RECT*>(lParam);
                snapSize(rect, native->dragStart, cursorSince(native->dragCursor), wParam);
                clampEdgesToWorkArea(rect, wParam);
                enforceMinimumSize(rect, wParam, native->minWidth, native->minHeight);
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_EXITSIZEMOVE:
            native->snapping = false;
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                native->owner->notifyFocusLost();
            }
            return 0;
        case WM_CLOSE:
            if (!native->owner->requestClose()) {
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_DESTROY:
            native->hwnd = nullptr;
            if (!native->destroying) {
                native->owner->notifyClosed();
            }
            return 0;
        case WM_ERASEBKGND:
            // Painting the background would cover the desktop that a see-through window shows.
            if (native->transparent) {
                return 1;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_SETCURSOR:
            if (native->compositionController && LOWORD(lParam) == HTCLIENT) {
                SetCursor(native->cursor ? native->cursor : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (native->compositionController) {
                forwardMouseInput(native, message, wParam, lParam);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void registerWindowClass() {
    static const bool registered = [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, as a wide-string resource id
        // Shows until the page has drawn, so the window doesn't flash white on start.
        windowClass.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            throw std::runtime_error("The window class couldn't be registered.");
        }
        return true;
    }();
    (void)registered;
}

void ensureComInitialized() {
    static const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("COM couldn't be initialized.");
    }
}

}  // namespace

void NativeWindowDeleter::operator()(NativeWindow* native) const {
    if (!native) {
        return;
    }
    native->destroying = true;
    if (native->transparent && native->hwnd) {
        composition::revokeDropTarget(native->hwnd);
    }
    if (native->webview) {
        native->webview->Release();
    }
    if (native->controller) {
        native->controller->Close();
        native->controller->Release();
    }
    if (native->compositionController) {
        native->compositionController->Release();
    }
    if (native->hwnd) {
        DestroyWindow(native->hwnd);
    }
    delete native;
}

namespace {

// A frameless popup. A resizable one keeps the sizing frame, since Windows only runs the size loop for a window that has it;
// WM_NCCALCSIZE gives the frame no room. The system menu and minimise box let the taskbar minimise and restore the window.
DWORD windowStyle(const WindowOptions& options) {
    DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX;
    if (options.resizable && !options.autosize) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    }
    return style;
}

// Starts the system's move or size loop, as a press on the frame would. The page's press leaves the mouse captured, so the
// capture is released first.
void beginSystemDrag(HWND hwnd, WPARAM hitTest) {
    POINT point{};
    GetCursorPos(&point);
    ReleaseCapture();
    PostMessageW(hwnd, WM_NCLBUTTONDOWN, hitTest, MAKELPARAM(point.x, point.y));
}

// The frame's hit-test code for each ResizeEdge, in its order.
const WPARAM kEdgeHitTests[] = {HTTOPLEFT, HTTOP, HTTOPRIGHT, HTLEFT, HTRIGHT, HTBOTTOMLEFT, HTBOTTOM, HTBOTTOMRIGHT};

// A hidden window that owns the windows kept off the taskbar. A window with an owner gets no taskbar button, and unlike the
// tool-window style it still minimises normally.
HWND taskbarOwner() {
    static HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    return owner;
}

// Gives an open window its owner, or none. The taskbar reads the owner when a window is shown, so a visible window is hidden and shown again.
void applySkipTaskbar(HWND hwnd, bool skip) {
    const bool visible = IsWindowVisible(hwnd);
    if (visible) {
        ShowWindow(hwnd, SW_HIDE);
    }
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(skip ? taskbarOwner() : nullptr));
    if (visible) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
}

}  // namespace

void WebViewWindow::platformSetSkipTaskbar() {
    if (native_ && native_->hwnd) {
        applySkipTaskbar(native_->hwnd, options_.skipTaskbar);
    }
}

void WebViewWindow::platformBeginMove() {
    if (native_ && native_->hwnd) {
        if (native_->compositionController) {
            // The press was forwarded to the page, so its point is known. The window follows the cursor from there while the button is held.
            native_->manualMove = true;
            return;
        }
        native_->snapping = true;
        beginSystemDrag(native_->hwnd, HTCAPTION);
    }
}

void WebViewWindow::platformBeginResize(ResizeEdge edge) {
    if (native_ && native_->hwnd) {
        native_->snapping = true;
        beginSystemDrag(native_->hwnd, kEdgeHitTests[static_cast<int>(edge)]);
    }
}

void WebViewWindow::platformMinimize() {
    if (native_ && native_->hwnd) {
        ShowWindow(native_->hwnd, SW_MINIMIZE);
    }
}

void WebViewWindow::platformToggleMaximize() {
    if (native_ && native_->hwnd) {
        ShowWindow(native_->hwnd, IsZoomed(native_->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }
}

// Posts the close the title bar's button would, so WM_CLOSE asks requestClose.
void WebViewWindow::platformClose() {
    if (native_ && native_->hwnd) {
        PostMessageW(native_->hwnd, WM_CLOSE, 0, 0);
    }
}

void WebViewWindow::platformOpen() {
    ensureComInitialized();
    registerWindowClass();

    const WindowOptions& options = options_;
    DWORD style = windowStyle(options);
    DWORD exStyle = 0;
    if (options.alwaysOnTop) {
        exStyle |= WS_EX_TOPMOST;
    }
    if (options.transparent) {
        // The window has no surface of its own, so what the composition draws over it is all that shows.
        exStyle |= WS_EX_NOREDIRECTIONBITMAP;
    }

    native_.reset(new NativeWindow);
    NativeWindow* native = native_.get();
    native->owner = this;
    std::wstring windowTitle = toWide(title());
    // Opened in the middle of the work area. A caller that places the window moves it after it's opened.
    const auto area = workArea();
    // Sizes are in device-independent pixels, so they grow with the display's DPI.
    const UINT dpi = GetDpiForSystem();
    native->minWidth = MulDiv(scaledSize(options.minWidth), dpi, 96);
    native->minHeight = MulDiv(scaledSize(options.minHeight), dpi, 96);
    const int width = std::max(MulDiv(scaledSize(options.width), dpi, 96), native->minWidth);
    const int height = std::max(MulDiv(scaledSize(options.height), dpi, 96), native->minHeight);
    const int x = area ? area->x + (area->width - width) / 2 : CW_USEDEFAULT;
    const int y = area ? area->y + (area->height - height) / 2 : CW_USEDEFAULT;
    // The window has no frame that takes room, so its size is the page's size.
    native->hwnd = CreateWindowExW(exStyle, kWindowClass, windowTitle.c_str(), style, x, y, width, height,
                                   options.skipTaskbar ? taskbarOwner() : nullptr, nullptr, GetModuleHandleW(nullptr), native);
    if (!native->hwnd) {
        throw std::runtime_error("The window couldn't be created.");
    }
    if (options.transparent) {
        native->transparent = true;
        native->surface.create(native->hwnd);
    }

    const std::string url = devUrl();
    const std::string page = html();
    auto onEnvironment = [native, url, page](HRESULT result, ICoreWebView2Environment* environment) {
        if (FAILED(result) || !environment) {
            showError("The web view couldn't start, so the window has been closed. Is the WebView2 runtime installed?");
            native->owner->close();
            return;
        }
        auto navigate = [native, url, page] {
            if (!url.empty()) {
                native->webview->Navigate(toWide(url).c_str());
            } else {
                native->webview->NavigateToString(toWide(page).c_str());
            }
        };
        if (native->transparent) {
            ICoreWebView2Environment3* environment3 = nullptr;
            if (FAILED(environment->QueryInterface(IID_ICoreWebView2Environment3, reinterpret_cast<void**>(&environment3)))) {
                showError("The see-through page needs a newer WebView2 runtime, so the window has been closed.");
                native->owner->close();
                return;
            }
            environment3->CreateCoreWebView2CompositionController(
                native->hwnd,
                webview2::compositionControllerDone([native, navigate](HRESULT controllerResult,
                                                                       ICoreWebView2CompositionController* compositionController) {
                    ICoreWebView2Controller* controller = nullptr;
                    if (FAILED(controllerResult) || !compositionController ||
                        FAILED(compositionController->QueryInterface(IID_ICoreWebView2Controller, reinterpret_cast<void**>(&controller)))) {
                        showError("The web view couldn't be attached to its window, so the window has been closed.");
                        native->owner->close();
                        return;
                    }
                    attachCompositionWebView(native, compositionController, controller);
                    controller->Release();
                    navigate();
                }));
            environment3->Release();
            return;
        }
        environment->CreateCoreWebView2Controller(
            native->hwnd, webview2::controllerDone([native, navigate](HRESULT controllerResult, ICoreWebView2Controller* controller) {
                if (FAILED(controllerResult) || !controller) {
                    showError("The web view couldn't be attached to its window, so the window has been closed.");
                    native->owner->close();
                    return;
                }
                attachWebView(native, controller);
                navigate();
            }));
    };
    auto createEnvironment = loaderFunction<CreateEnvironmentFn>("CreateCoreWebView2EnvironmentWithOptions");
    createEnvironment(nullptr, nullptr, nullptr, webview2::environmentDone(onEnvironment));

    if (options.keepBelow) {
        SetWindowPos(native->hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(native->hwnd, options.visible ? SW_SHOW : SW_HIDE);
}

void WebViewWindow::platformPresent() {
    if (native_ && native_->hwnd) {
        // Restores the window if it's minimised, then asks for the foreground. Windows may keep the focus where it is.
        ShowWindow(native_->hwnd, IsIconic(native_->hwnd) ? SW_RESTORE : SW_SHOW);
        syncWebViewVisibility(native_.get());
        SetForegroundWindow(native_->hwnd);
    }
}

void WebViewWindow::platformShow(bool visible) {
    if (native_ && native_->hwnd) {
        ShowWindow(native_->hwnd, visible ? SW_SHOW : SW_HIDE);
        syncWebViewVisibility(native_.get());
    }
}

void WebViewWindow::platformResize(int width, int height) {
    if (native_ && native_->hwnd) {
        const UINT dpi = GetDpiForWindow(native_->hwnd);
        SetWindowPos(native_->hwnd, nullptr, 0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

int WebViewWindow::platformToIndependent(int pixels) const {
    if (!native_ || !native_->hwnd) {
        return pixels;
    }
    return MulDiv(pixels, 96, GetDpiForWindow(native_->hwnd));
}

std::optional<Rect> WebViewWindow::platformFrame() const {
    // A minimised window reports an off-screen place, which isn't where it is.
    if (!native_ || !native_->hwnd || IsIconic(native_->hwnd)) {
        return std::nullopt;
    }
    RECT rect{};
    if (!GetWindowRect(native_->hwnd, &rect)) {
        return std::nullopt;
    }
    return Rect{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
}

std::optional<RuntimeProblem> missingWindowingRuntime() {
    // The loader is found beside the executable, and it reports the runtime's version when the runtime is installed.
    HMODULE loader = LoadLibraryW((executableFolder() / "WebView2Loader.dll").c_str());
    if (!loader) {
        return RuntimeProblem{"WebView2Loader.dll isn't next to NoFences.exe, so its windows can't open.", ""};
    }
    using VersionFn = HRESULT(WINAPI*)(LPCWSTR, LPWSTR*);
    auto version = reinterpret_cast<VersionFn>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString"));
    LPWSTR text = nullptr;
    if (!version || FAILED(version(nullptr, &text)) || !text) {
        return RuntimeProblem{"The Microsoft Edge WebView2 runtime isn't installed, so NoFences' windows can't open. Install it, then start NoFences again.", kWebView2Download};
    }
    CoTaskMemFree(text);
    return std::nullopt;
}

void offerLink(const std::string& message, const std::string& link) {
    const std::wstring text = toWide(message + "\n\nOpen the WebView2 download page now?");
    if (MessageBoxW(nullptr, text.c_str(), L"NoFences", MB_YESNO | MB_ICONERROR) == IDYES) {
        const std::wstring url = toWide(link);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void initializeWindowing() {
    requireWindowingRuntime();
}

// The page's background is see-through: the composition draws it over the desktop, and the blur behind the window shows through.
bool transparentBackgroundSupported() {
    return true;
}

std::filesystem::path executableFolder() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(std::wstring(path, length)).parent_path();
}

void WebViewWindow::platformMove(int x, int y) {
    if (native_ && native_->hwnd) {
        SetWindowPos(native_->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void WebViewWindow::platformSetTitle(const std::string& title) {
    if (native_ && native_->hwnd) {
        std::wstring wide = toWide(title);
        SetWindowTextW(native_->hwnd, wide.c_str());
    }
}

void WebViewWindow::platformOpenDevTools() {
    if (native_ && native_->webview) {
        native_->webview->OpenDevToolsWindow();
    }
}

void WebViewWindow::platformRunScript(const std::string& script) {
    if (!native_) {
        return;
    }
    std::wstring wide = toWide(script);
    if (native_->webview) {
        native_->webview->ExecuteScript(wide.c_str(), webview2::scriptDone());
    } else {
        native_->pendingScripts.push_back(std::move(wide));
    }
}

namespace {

const UINT kRunTask = WM_APP + 1;
DWORD gUiThread = 0;

}  // namespace

void runEventLoop() {
    gUiThread = GetCurrentThreadId();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kRunTask && !message.hwnd) {
            auto* task = reinterpret_cast<std::function<void()>*>(message.lParam);
            (*task)();
            delete task;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void runOnUiThread(std::function<void()> task) {
    PostThreadMessageW(gUiThread, kRunTask, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(task))));
}

void stopEventLoop() {
    PostQuitMessage(0);
}

}  // namespace gwl
