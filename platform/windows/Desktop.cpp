// Windows: the pointer, the work area, the error dialog, the desktop folder, Steam, icons and the single-instance mutex.

#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Entries.h"
#include "Platform.h"
#include "WebViewWindow.h"

namespace {

constexpr int kIconSize = 32;

std::wstring toWide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(length > 0 ? length : 1, L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    }
    return wide;
}

// GDI+ is started the first time an icon is wanted, and stays on until the program exits.
bool gdiPlusReady() {
    static const bool started = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return started;
}

std::optional<CLSID> pngEncoder() {
    UINT count = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    std::vector<unsigned char> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            return encoders[i].Clsid;
        }
    }
    return std::nullopt;
}

// The image as PNG bytes. The stream's length is the bytes written, since its memory block can be larger.
std::optional<std::vector<unsigned char>> savePng(Gdiplus::Image& image) {
    const auto encoder = pngEncoder();
    IStream* stream = nullptr;
    if (!encoder || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return std::nullopt;
    }
    std::optional<std::vector<unsigned char>> png;
    if (image.Save(stream, &*encoder, nullptr) == Gdiplus::Ok) {
        ULARGE_INTEGER length{};
        LARGE_INTEGER origin{};
        HGLOBAL memory = nullptr;
        if (SUCCEEDED(stream->Seek(origin, STREAM_SEEK_END, &length)) && SUCCEEDED(GetHGlobalFromStream(stream, &memory))) {
            const auto* bytes = static_cast<const unsigned char*>(GlobalLock(memory));
            if (bytes) {
                png = std::vector<unsigned char>(bytes, bytes + length.QuadPart);
                GlobalUnlock(memory);
            }
        }
    }
    stream->Release();
    return png;
}

}  // namespace

namespace gwl {

void showError(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    MessageBoxW(nullptr, toWide(message).c_str(), L"NoFences", MB_OK | MB_ICONERROR);
}

std::optional<std::pair<int, int>> cursorPosition() {
    POINT point{};
    if (!GetCursorPos(&point)) {
        return std::nullopt;
    }
    return std::make_pair(static_cast<int>(point.x), static_cast<int>(point.y));
}

// The work area: the screen less the taskbar.
std::optional<Rect> workArea() {
    RECT area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) {
        return std::nullopt;
    }
    return Rect{area.left, area.top, area.right - area.left, area.bottom - area.top};
}

// The work area of the monitor that holds most of the rectangle, or the nearest one when it's off every monitor.
std::optional<Rect> workAreaAt(const Rect& rect) {
    const RECT bounds{rect.x, rect.y, rect.right(), rect.bottom()};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
        return std::nullopt;
    }
    const RECT& area = info.rcWork;
    return Rect{area.left, area.top, area.right - area.left, area.bottom - area.top};
}

}  // namespace gwl

namespace nofences {

std::filesystem::path defaultDataDirectory() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return std::filesystem::path(local ? local : L".") / L"NoFences";
}

std::filesystem::path desktopDirectory() {
    wchar_t path[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, path) == S_OK) {
        return path;
    }
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    return profile ? std::filesystem::path(profile) : std::filesystem::path(".");
}

void openTarget(const std::string& target) {
    // A link has no folder to start in, so the program is started with none.
    const std::wstring wideTarget = toWide(target);
    const std::wstring folder = isLink(target) ? std::wstring() : pathFromUtf8(target).parent_path().wstring();
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wideTarget.c_str(), nullptr, folder.empty() ? nullptr : folder.c_str(), SW_SHOWNORMAL));
    if (result > 32) {
        return;
    }
    // ShellExecute's codes at or below 32 are failures; the ones NoFences can meet get a plain reason.
    std::string reason;
    switch (result) {
        case SE_ERR_NOASSOC:
            reason = "no program is set up to open it";
            break;
        case SE_ERR_FNF:
        case SE_ERR_PNF:
            reason = "it couldn't be found";
            break;
        case SE_ERR_ACCESSDENIED:
            reason = "access was denied";
            break;
        default:
            reason = "Windows reported error " + std::to_string(result);
    }
    throw std::runtime_error("NoFences couldn't open " + target + ": " + reason + ".");
}

std::vector<std::filesystem::path> steamRoots() {
    std::vector<std::filesystem::path> roots;
    wchar_t path[1024] = {};
    DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS) {
        roots.emplace_back(path);
    }
    return roots;
}

std::optional<std::vector<unsigned char>> pictureIconPng(const std::filesystem::path& picture) {
    if (!gdiPlusReady()) {
        return std::nullopt;
    }
    Gdiplus::Bitmap source(picture.c_str());
    if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() == 0 || source.GetHeight() == 0) {
        return std::nullopt;
    }
    // Scaled to fit the square, centred, so a tall or wide picture keeps its shape.
    const float scale = std::min(static_cast<float>(kIconSize) / source.GetWidth(), static_cast<float>(kIconSize) / source.GetHeight());
    const float width = source.GetWidth() * scale;
    const float height = source.GetHeight() * scale;
    Gdiplus::Bitmap canvas(kIconSize, kIconSize, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&canvas);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(&source, (kIconSize - width) / 2, (kIconSize - height) / 2, width, height);
    return savePng(canvas);
}

// The icon's pixels with their alpha channel. GDI+'s own conversion of an icon drops the alpha, so transparent pixels come out black.
// An icon with no alpha at all (an older, mask-only one) is converted the usual way.
std::unique_ptr<Gdiplus::Bitmap> iconBitmap(HICON icon) {
    ICONINFO iconInfo{};
    if (GetIconInfo(icon, &iconInfo) && iconInfo.hbmColor) {
        BITMAP size{};
        GetObjectW(iconInfo.hbmColor, sizeof(size), &size);
        BITMAPINFO header{};
        header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        header.bmiHeader.biWidth = size.bmWidth;
        header.bmiHeader.biHeight = -size.bmHeight;  // top-down rows
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> pixels(static_cast<size_t>(size.bmWidth) * size.bmHeight * 4);
        HDC screen = GetDC(nullptr);
        const int lines = GetDIBits(screen, iconInfo.hbmColor, 0, size.bmHeight, pixels.data(), &header, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        DeleteObject(iconInfo.hbmColor);
        DeleteObject(iconInfo.hbmMask);
        bool hasAlpha = false;
        for (size_t i = 3; i < pixels.size(); i += 4) {
            hasAlpha = hasAlpha || pixels[i] != 0;
        }
        if (lines == size.bmHeight && hasAlpha) {
            // The bitmap only borrows the pixels, so it's copied before they go.
            Gdiplus::Bitmap source(size.bmWidth, size.bmHeight, size.bmWidth * 4, PixelFormat32bppARGB, pixels.data());
            if (Gdiplus::Bitmap* copy = source.Clone(0, 0, size.bmWidth, size.bmHeight, PixelFormat32bppARGB)) {
                return std::unique_ptr<Gdiplus::Bitmap>(copy);
            }
        }
    }
    return std::make_unique<Gdiplus::Bitmap>(icon);
}

std::optional<std::vector<unsigned char>> systemIconPng(const std::string& entry) {
    if (!gdiPlusReady()) {
        return std::nullopt;
    }
    std::error_code error;
    // The shell's icon. The file's type decides it, so a link and a folder use their type's icon without touching the disk.
    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    DWORD attributes = 0;
    std::wstring path = toWide(entry);
    if (isLink(entry)) {
        path = L"link.url";
        attributes = FILE_ATTRIBUTE_NORMAL;
        flags |= SHGFI_USEFILEATTRIBUTES;
    } else if (std::filesystem::is_directory(pathFromUtf8(entry), error)) {
        attributes = FILE_ATTRIBUTE_DIRECTORY;
        flags |= SHGFI_USEFILEATTRIBUTES;
    }
    if (!SHGetFileInfoW(path.c_str(), attributes, &info, sizeof(info), flags) || !info.hIcon) {
        return std::nullopt;
    }
    const auto icon = iconBitmap(info.hIcon);
    DestroyIcon(info.hIcon);
    return savePng(*icon);
}

std::optional<std::vector<unsigned char>> themeIconPng(const std::string&) {
    return std::nullopt;
}

bool claimSingleInstance(const std::filesystem::path&) {
    // The mutex is kept open until the program exits, which is what keeps the second start out.
    static const bool first = [] {
        CreateMutexW(nullptr, TRUE, L"NoFences.SingleInstance");
        return GetLastError() != ERROR_ALREADY_EXISTS;
    }();
    return first;
}

}  // namespace nofences
