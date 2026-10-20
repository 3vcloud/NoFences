// Linux: the pointer, the work area, the error dialog, the desktop folder, Steam, icons and the single-instance lock. GTK and
// GdkPixbuf are opened at runtime, as the windows are, so the icons and the pointer are missing when GTK isn't installed.

#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Entries.h"
#include "Platform.h"
#include "Text.h"
#include "WebViewWindow.h"

extern char** environ;

namespace {

constexpr int kIconSize = 32;
// GTK_ICON_LOOKUP_USE_BUILTIN and GTK_ICON_LOOKUP_FORCE_SIZE, so an icon comes out exactly kIconSize across.
constexpr int kIconLookupFlags = (1 << 2) | (1 << 4);
// GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR and GTK_BUTTONS_OK.
constexpr int kDialogModal = 1;
constexpr int kMessageError = 3;
constexpr int kButtonsOk = 1;

struct GdkRectangle {
    int x;
    int y;
    int width;
    int height;
};

// The GTK, GIO and GdkPixbuf functions used here. Objects are void*, since only their addresses are needed.
struct GtkLibrary {
    int (*initCheck)(int*, char***) = nullptr;
    void* (*messageDialogNew)(void*, int, int, int, const char*, ...) = nullptr;
    int (*dialogRun)(void*) = nullptr;
    void (*widgetDestroy)(void*) = nullptr;
    void* (*displayGetDefault)() = nullptr;
    void (*displayGetPointer)(void*, void**, int*, int*, int*) = nullptr;
    void* (*displayGetPrimaryMonitor)(void*) = nullptr;
    void (*monitorGetWorkarea)(void*, GdkRectangle*) = nullptr;
    void* (*iconThemeGetDefault)() = nullptr;
    void* (*iconThemeLookupByGIcon)(void*, void*, int, int) = nullptr;
    void* (*iconInfoLoadIcon)(void*, void**) = nullptr;
    void* (*pixbufFromFileAtScale)(const char*, int, int, int, void**) = nullptr;
    int (*pixbufSaveToBufferv)(void*, char**, std::size_t*, const char*, char**, char**, void**) = nullptr;
    void* (*fileNewForPath)(const char*) = nullptr;
    void* (*fileQueryInfo)(void*, const char*, int, void*, void**) = nullptr;
    void* (*fileInfoGetIcon)(void*) = nullptr;
    void* (*themedIconNew)(const char*) = nullptr;
    void* (*appInfoDefaultForUriScheme)(const char*) = nullptr;
    void* (*appInfoGetIcon)(void*) = nullptr;
    void* (*objectRef)(void*) = nullptr;
    void (*objectUnref)(void*) = nullptr;
    void (*memoryFree)(void*) = nullptr;
};

template <typename F>
F symbolOf(void* library, const char* name) {
    return reinterpret_cast<F>(dlsym(library, name));
}

// Opened on first use. Null when GTK isn't installed, or there's no display to draw on.
const GtkLibrary* gtkLibrary() {
    static const GtkLibrary* loaded = [] () -> const GtkLibrary* {
        void* handle = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            return nullptr;
        }
        static GtkLibrary library;
        // The GIO, GdkPixbuf and GLib functions are in libraries GTK depends on, which dlsym searches too.
        library.initCheck = symbolOf<decltype(library.initCheck)>(handle, "gtk_init_check");
        library.messageDialogNew = symbolOf<decltype(library.messageDialogNew)>(handle, "gtk_message_dialog_new");
        library.dialogRun = symbolOf<decltype(library.dialogRun)>(handle, "gtk_dialog_run");
        library.widgetDestroy = symbolOf<decltype(library.widgetDestroy)>(handle, "gtk_widget_destroy");
        library.displayGetDefault = symbolOf<decltype(library.displayGetDefault)>(handle, "gdk_display_get_default");
        library.displayGetPointer = symbolOf<decltype(library.displayGetPointer)>(handle, "gdk_display_get_pointer");
        library.displayGetPrimaryMonitor =
            symbolOf<decltype(library.displayGetPrimaryMonitor)>(handle, "gdk_display_get_primary_monitor");
        library.monitorGetWorkarea = symbolOf<decltype(library.monitorGetWorkarea)>(handle, "gdk_monitor_get_workarea");
        library.iconThemeGetDefault = symbolOf<decltype(library.iconThemeGetDefault)>(handle, "gtk_icon_theme_get_default");
        library.iconThemeLookupByGIcon =
            symbolOf<decltype(library.iconThemeLookupByGIcon)>(handle, "gtk_icon_theme_lookup_by_gicon");
        library.iconInfoLoadIcon = symbolOf<decltype(library.iconInfoLoadIcon)>(handle, "gtk_icon_info_load_icon");
        library.pixbufFromFileAtScale =
            symbolOf<decltype(library.pixbufFromFileAtScale)>(handle, "gdk_pixbuf_new_from_file_at_scale");
        library.pixbufSaveToBufferv = symbolOf<decltype(library.pixbufSaveToBufferv)>(handle, "gdk_pixbuf_save_to_bufferv");
        library.fileNewForPath = symbolOf<decltype(library.fileNewForPath)>(handle, "g_file_new_for_path");
        library.fileQueryInfo = symbolOf<decltype(library.fileQueryInfo)>(handle, "g_file_query_info");
        library.fileInfoGetIcon = symbolOf<decltype(library.fileInfoGetIcon)>(handle, "g_file_info_get_icon");
        library.themedIconNew = symbolOf<decltype(library.themedIconNew)>(handle, "g_themed_icon_new");
        library.appInfoDefaultForUriScheme =
            symbolOf<decltype(library.appInfoDefaultForUriScheme)>(handle, "g_app_info_get_default_for_uri_scheme");
        library.appInfoGetIcon = symbolOf<decltype(library.appInfoGetIcon)>(handle, "g_app_info_get_icon");
        library.objectRef = symbolOf<decltype(library.objectRef)>(handle, "g_object_ref");
        library.objectUnref = symbolOf<decltype(library.objectUnref)>(handle, "g_object_unref");
        library.memoryFree = symbolOf<decltype(library.memoryFree)>(handle, "g_free");

        const void* required[] = {
            reinterpret_cast<const void*>(library.initCheck),
            reinterpret_cast<const void*>(library.messageDialogNew),
            reinterpret_cast<const void*>(library.dialogRun),
            reinterpret_cast<const void*>(library.widgetDestroy),
            reinterpret_cast<const void*>(library.displayGetDefault),
            reinterpret_cast<const void*>(library.displayGetPointer),
            reinterpret_cast<const void*>(library.displayGetPrimaryMonitor),
            reinterpret_cast<const void*>(library.monitorGetWorkarea),
            reinterpret_cast<const void*>(library.iconThemeGetDefault),
            reinterpret_cast<const void*>(library.iconThemeLookupByGIcon),
            reinterpret_cast<const void*>(library.iconInfoLoadIcon),
            reinterpret_cast<const void*>(library.pixbufFromFileAtScale),
            reinterpret_cast<const void*>(library.pixbufSaveToBufferv),
            reinterpret_cast<const void*>(library.fileNewForPath),
            reinterpret_cast<const void*>(library.fileQueryInfo),
            reinterpret_cast<const void*>(library.fileInfoGetIcon),
            reinterpret_cast<const void*>(library.themedIconNew),
            reinterpret_cast<const void*>(library.appInfoDefaultForUriScheme),
            reinterpret_cast<const void*>(library.appInfoGetIcon),
            reinterpret_cast<const void*>(library.objectRef),
            reinterpret_cast<const void*>(library.objectUnref),
            reinterpret_cast<const void*>(library.memoryFree),
        };
        for (const void* pointer : required) {
            if (!pointer) {
                return nullptr;
            }
        }
        if (!library.initCheck(nullptr, nullptr)) {
            return nullptr;
        }
        return &library;
    }();
    return loaded;
}

// Shows the message with zenity, for when GTK isn't installed.
void showWithZenity(const std::string& message) {
    const char* argv[] = {"zenity", "--error", "--title=NoFences", "--text", message.c_str(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "zenity", nullptr, nullptr, const_cast<char**>(argv), environ) != 0) {
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

std::filesystem::path homeDirectory() {
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path(".");
}

// The icon for an entry that has no picture of its own: the app for a link's scheme, a folder, or the file's type.
void* fallbackIcon(const GtkLibrary& gtk, const std::string& entry) {
    if (nofences::isLink(entry)) {
        const std::string scheme = entry.substr(0, entry.find("://"));
        if (void* app = gtk.appInfoDefaultForUriScheme(scheme.c_str())) {
            void* icon = gtk.appInfoGetIcon(app);
            void* owned = icon ? gtk.objectRef(icon) : nullptr;
            gtk.objectUnref(app);
            if (owned) {
                return owned;
            }
        }
        return gtk.themedIconNew("applications-internet");
    }
    std::error_code error;
    if (std::filesystem::is_directory(nofences::pathFromUtf8(entry), error)) {
        return gtk.themedIconNew("folder");
    }
    if (void* file = gtk.fileNewForPath(entry.c_str())) {
        void* info = gtk.fileQueryInfo(file, "standard::icon", 0, nullptr, nullptr);
        gtk.objectUnref(file);
        if (info) {
            void* icon = gtk.fileInfoGetIcon(info);
            void* owned = icon ? gtk.objectRef(icon) : nullptr;
            gtk.objectUnref(info);
            if (owned) {
                return owned;
            }
        }
    }
    return gtk.themedIconNew("text-x-generic");
}

void* themedPixbuf(const GtkLibrary& gtk, void* icon) {
    void* info = gtk.iconThemeLookupByGIcon(gtk.iconThemeGetDefault(), icon, kIconSize, kIconLookupFlags);
    if (!info) {
        return nullptr;
    }
    void* pixbuf = gtk.iconInfoLoadIcon(info, nullptr);
    gtk.objectUnref(info);
    return pixbuf;
}

}  // namespace

namespace gwl {

void showError(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    if (const GtkLibrary* gtk = gtkLibrary()) {
        void* dialog = gtk->messageDialogNew(nullptr, kDialogModal, kMessageError, kButtonsOk, "%s", message.c_str());
        gtk->dialogRun(dialog);
        gtk->widgetDestroy(dialog);
        return;
    }
    showWithZenity(message);
}

std::optional<std::pair<int, int>> cursorPosition() {
    const GtkLibrary* gtk = gtkLibrary();
    void* display = gtk ? gtk->displayGetDefault() : nullptr;
    if (!display) {
        return std::nullopt;
    }
    int x = 0;
    int y = 0;
    gtk->displayGetPointer(display, nullptr, &x, &y, nullptr);
    return std::make_pair(x, y);
}

// The primary monitor's work area: the screen less the panels.
std::optional<Rect> workArea() {
    const GtkLibrary* gtk = gtkLibrary();
    void* display = gtk ? gtk->displayGetDefault() : nullptr;
    void* monitor = display ? gtk->displayGetPrimaryMonitor(display) : nullptr;
    if (!monitor) {
        return std::nullopt;
    }
    GdkRectangle area{};
    gtk->monitorGetWorkarea(monitor, &area);
    return Rect{area.x, area.y, area.width, area.height};
}

// Windows aren't kept on a monitor's work area here, since the monitor under a window isn't looked up on Linux.
std::optional<Rect> workAreaAt(const Rect&) {
    return std::nullopt;
}

}  // namespace gwl

namespace nofences {

std::filesystem::path defaultDataDirectory() {
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome && *dataHome) {
        return std::filesystem::path(dataHome) / "NoFences";
    }
    return homeDirectory() / ".local" / "share" / "NoFences";
}

// The desktop folder the user set up in user-dirs.dirs, which the desktop writes when it creates the home folder.
std::filesystem::path desktopDirectory() {
    const auto home = homeDirectory();
    const std::string key = "XDG_DESKTOP_DIR=\"";
    if (const auto dirs = readFile(home / ".config" / "user-dirs.dirs")) {
        std::istringstream lines(*dirs);
        std::string line;
        while (std::getline(lines, line)) {
            if (startsWith(line, key) && line.size() > key.size() && line.back() == '"') {
                std::string value = line.substr(key.size(), line.size() - key.size() - 1);
                if (startsWith(value, "$HOME")) {
                    value = pathToUtf8(home) + value.substr(5);
                }
                return pathFromUtf8(value);
            }
        }
    }
    const auto desktop = home / "Desktop";
    std::error_code error;
    return std::filesystem::is_directory(desktop, error) ? desktop : home;
}

void openTarget(const std::string& target) {
    // A steam:// link is given to steam itself, as the shortcuts Steam makes do: GIO can't find a handler for that scheme.
    // xdg-open shows a desktop entry's text in an editor, so the entry is launched as the desktop would launch it.
    const bool desktopEntry = !isLink(target) && pathFromUtf8(target).extension() == ".desktop";
    const std::vector<std::string> words = startsWith(target, "steam://") ? std::vector<std::string>{"steam", target}
                                           : desktopEntry             ? std::vector<std::string>{"gio", "launch", target}
                                                                      : std::vector<std::string>{"xdg-open", target};
    const std::string& program = words[0];

    // The program's errors go to a file rather than a pipe: a program it starts can outlive it and would hold a pipe open.
    std::string errorFile = pathToUtf8(std::filesystem::temp_directory_path() / "nofences-open-XXXXXX");
    const int errorDescriptor = mkstemp(errorFile.data());
    if (errorDescriptor < 0) {
        throw std::runtime_error("NoFences couldn't open " + target + ": no temporary file could be made (" + std::strerror(errno) + ").");
    }
    std::vector<char*> argv;
    for (const std::string& word : words) {
        argv.push_back(const_cast<char*>(word.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, errorDescriptor, STDERR_FILENO);
    pid_t pid = 0;
    const int started = posix_spawnp(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(errorDescriptor);
    if (started != 0) {
        std::error_code error;
        std::filesystem::remove(errorFile, error);
        throw std::runtime_error("NoFences couldn't start " + program + " to open " + target + ": " + std::strerror(started) + ".");
    }

    // Waited for on a thread of its own, so the program doesn't stay behind as a zombie. A failure is shown once it's known.
    std::thread([pid, target, errorFile] {
        int status = 0;
        waitpid(pid, &status, 0);
        const bool succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::string output;
        if (!succeeded) {
            output = trim(readFile(errorFile).value_or(std::string()));
        }
        std::error_code error;
        std::filesystem::remove(errorFile, error);
        if (!succeeded) {
            gwl::runOnUiThread([target, output] {
                gwl::showError("NoFences couldn't open " + target + (output.empty() ? "." : ":\n" + output));
            });
        }
    }).detach();
}

std::vector<std::filesystem::path> steamRoots() {
    const auto home = homeDirectory();
    return {home / ".steam" / "steam", home / ".local" / "share" / "Steam"};
}

namespace {

// The pixbuf as PNG bytes. The pixbuf is released, whether or not it could be saved.
std::optional<std::vector<unsigned char>> pixbufToPng(const GtkLibrary& gtk, void* pixbuf) {
    if (!pixbuf) {
        return std::nullopt;
    }
    char* buffer = nullptr;
    std::size_t size = 0;
    const bool saved = gtk.pixbufSaveToBufferv(pixbuf, &buffer, &size, "png", nullptr, nullptr, nullptr) != 0;
    gtk.objectUnref(pixbuf);
    if (!saved || !buffer) {
        return std::nullopt;
    }
    std::vector<unsigned char> png(buffer, buffer + size);
    gtk.memoryFree(buffer);
    return png;
}

}  // namespace

std::optional<std::vector<unsigned char>> pictureIconPng(const std::filesystem::path& picture) {
    const GtkLibrary* gtk = gtkLibrary();
    if (!gtk) {
        return std::nullopt;
    }
    // GdkPixbuf keeps the picture's shape when the size is a bounding box.
    const std::string path = pathToUtf8(picture);
    return pixbufToPng(*gtk, gtk->pixbufFromFileAtScale(path.c_str(), kIconSize, kIconSize, 1, nullptr));
}

std::optional<std::vector<unsigned char>> systemIconPng(const std::string& entry) {
    const GtkLibrary* gtk = gtkLibrary();
    if (!gtk) {
        return std::nullopt;
    }
    void* icon = fallbackIcon(*gtk, entry);
    void* pixbuf = themedPixbuf(*gtk, icon);
    gtk->objectUnref(icon);
    return pixbufToPng(*gtk, pixbuf);
}

std::optional<std::vector<unsigned char>> themeIconPng(const std::string& name) {
    const GtkLibrary* gtk = gtkLibrary();
    if (!gtk) {
        return std::nullopt;
    }
    void* icon = gtk->themedIconNew(name.c_str());
    void* pixbuf = themedPixbuf(*gtk, icon);
    gtk->objectUnref(icon);
    return pixbufToPng(*gtk, pixbuf);
}

bool claimSingleInstance(const std::filesystem::path& dataDirectory) {
    // The descriptor stays open, so the lock is held until the program exits.
    static const int descriptor = open((dataDirectory / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        gwl::showError("NoFences couldn't create its lock file in " + pathToUtf8(dataDirectory) + " (" + std::strerror(errno) +
                       "), so it can't tell whether another copy is running. It will start anyway.");
        return true;
    }
    return flock(descriptor, LOCK_EX | LOCK_NB) == 0;
}

}  // namespace nofences
