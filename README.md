# NoFences

Didn't want to pay 11€, made my own.

NoFences keeps desktop fences that hold shortcuts to files, folders and links. It's written in C++, and each fence is a
frameless window whose page is in `frontend/`. The window layer is shared with [GW Launcher](../gwdevhub_gwlauncher): WebKitGTK
on Linux and WebView2 on Windows.

![Screenshot](screenshot.png "NoFences in action")

## Build

    ./build.sh            # both targets
    ./build.sh linux      # bin/linux/NoFences
    ./build.sh windows    # bin/windows/NoFences.exe

The Linux build needs CMake, a C++20 compiler and nothing else: GTK and WebKitGTK 4.1 are opened at runtime. The Windows build
is cross-compiled with MinGW-w64 (`x86_64-w64-mingw32-g++`). The Windows executable needs the WebView2 runtime, which current
Windows 10 and 11 installs already have; the build copies `WebView2Loader.dll` beside it.

## Run

    bin/linux/NoFences

Fences are kept under `$XDG_DATA_HOME/NoFences` (`~/.local/share/NoFences` by default), or `%LOCALAPPDATA%\NoFences` on Windows.
Each fence is a folder holding `__fence_metadata.xml`, in the same format as the original Windows app, so existing fences carry
over. Set `NOFENCES_DATA_DIR` to use another folder.

For autostart on Linux, copy `bin/linux/NoFences.desktop` to `~/.config/autostart/`.

## Usage

- Drag files, folders or links onto a fence to add them. Right-click a fence's empty space, then Add Link..., to add a link by hand.
- Double-click an item to open it. Click to select.
- Right-click an item for Open, Show in Folder, Copy Path and Remove from Fence. Links get Copy Link instead.
- Right-click empty space for New Fence, Open Desktop Folder, Rename, Title Height, Locked, Minify When Not Hovered and Remove Fence.
- Shift + right-click always shows the fence menu.
- Drag the title bar to move, drag the edges to resize, and scroll the wheel if items overflow.
- Minified fences show only their title, and expand when the pointer comes back.

Only one copy runs at a time. A second start exits straight away.

## Layout

- `src/`, `include/`: the fence model, its XML store, the entries, the app and the fence window.
- `frontend/index.html`: the fence page, compiled into the executable.
- `platform/linux/`, `platform/windows/`: the windowing, the desktop facts (pointer, work area, icons, opening files, Steam
  libraries) and the single-instance lock for each platform.
- `src/WebViewWindow.cpp`, `src/Messages.cpp`, `include/WebViewWindow.h`, `platform/*/WebViewWindow.cpp`: copied from GW Launcher.
- `third_party/`: the GTK and WebKitGTK declarations, nlohmann/json and the WebView2 headers and loader, copied from GW Launcher.

## Differences from the original Windows app

- The Windows shell context menu is replaced by the page's own menu, so "Open With" isn't offered.
- Fences are see-through on both platforms. On Windows a fence draws its page through DirectComposition, and the blur behind it is applied to the window, as the original app did.
- Fences stay below other windows and off the taskbar on Linux. On Windows they start below other windows and stay off the taskbar, but a click can raise them.
- Dropping files on a fence depends on the web view handing over file addresses. That's tested on Linux only.
