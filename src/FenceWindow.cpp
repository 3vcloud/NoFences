#include "FenceWindow.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include "Entries.h"
#include "EmbeddedPage.h"
#include "FenceApp.h"
#include "Platform.h"
#include "Text.h"

namespace nofences {

namespace {

// The ResizeEdge values for the edges along the top of a fence. A minified fence is only as tall as its title, so these
// aren't offered.
constexpr int kTopEdges[] = {0, 1, 2};

WindowOptions windowOptions(const Fence& fence) {
    WindowOptions options;
    options.title = fence.name;
    options.width = fence.width;
    options.height = fence.canMinify ? fence.titleHeight : fence.height;
    options.minWidth = 100;
    options.minHeight = 100;
    options.skipTaskbar = true;
    options.keepBelow = true;
    options.sticky = true;
    options.utility = true;
    return options;
}

// The folders that hold the fence's files. A link has no folder, so it's left out.
std::set<std::string> foldersOf(const std::vector<std::string>& entries) {
    std::set<std::string> folders;
    for (const std::string& entry : entries) {
        if (!isLink(entry)) {
            folders.insert(pathToUtf8(pathFromUtf8(entry).parent_path()));
        }
    }
    return folders;
}

}  // namespace

FenceWindow::FenceWindow(FenceApp& app, Fence fence)
    : WebViewWindow(windowOptions(fence)),
      app_(app),
      fence_(std::move(fence)),
      minified_(fence_.canMinify),
      expandedHeight_(fence_.height) {
    registerMethods();
    folderWatch_ = std::make_unique<DirectoryWatch>([this] { onFolderChanged(); });
}

std::string FenceWindow::html() const {
    return embeddedPage();
}

void FenceWindow::registerMethods() {
    auto& methods = handler();

    methods.on("state", [this](WebViewWindow&, const Json&, const Reply& reply) { reply.result(state()); });

    methods.on("open", [](WebViewWindow&, const Json& args, const Reply& reply) {
        openTarget(args.at(0).get<std::string>());
        reply.result(nullptr);
    });

    methods.on("showInFolder", [](WebViewWindow&, const Json& args, const Reply& reply) {
        openTarget(pathToUtf8(pathFromUtf8(args.at(0).get<std::string>()).parent_path()));
        reply.result(nullptr);
    });

    methods.on("openDesktop", [](WebViewWindow&, const Json&, const Reply& reply) {
        openTarget(pathToUtf8(desktopDirectory()));
        reply.result(nullptr);
    });

    methods.on("openConfig", [this](WebViewWindow&, const Json&, const Reply& reply) {
        openTarget(pathToUtf8(app_.store().metadataPath(fence_)));
        reply.result(nullptr);
    });

    // Takes what the page was dropped: file addresses from the file manager, or links from the browser.
    // Says in problems what wasn't added, and why, so a drop that does nothing still tells the user.
    methods.on("addEntries", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        bool added = false;
        Json problems = Json::array();
        for (const Json& uri : args.at(0)) {
            const std::string text = uri.get<std::string>();
            const auto local = pathFromFileUri(text);
            const std::string entry = local ? *local : text;
            if (local && !entryExists(entry)) {
                problems.push_back("Nothing was added: " + entry + " doesn't exist.");
            } else if (!local && !isLink(text)) {
                problems.push_back("Nothing was added: " + text + " isn't a file or a link.");
            } else if (std::find(fence_.files.begin(), fence_.files.end(), entry) != fence_.files.end()) {
                problems.push_back("Nothing was added: " + entryLabel(entry) + " is already in this fence.");
            } else {
                fence_.files.push_back(entry);
                added = true;
            }
        }
        if (added) {
            save();
        }
        Json result = state();
        result["problems"] = problems;
        reply.result(result);
    });

    methods.on("addLink",[this](WebViewWindow&, const Json& args, const Reply& reply) {
        const std::string link = trim(args.at(0).get<std::string>());
        if (!isLink(link) || startsWith(link, "file://")) {
            reply.error("That isn't a link. Use an address such as steam://rungameid/1091500 or https://...");
            return;
        }
        if (std::find(fence_.files.begin(), fence_.files.end(), link) == fence_.files.end()) {
            fence_.files.push_back(link);
            save();
        }
        reply.result(state());
    });

    methods.on("remove", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        const std::string entry = args.at(0).get<std::string>();
        fence_.files.erase(std::remove(fence_.files.begin(), fence_.files.end(), entry), fence_.files.end());
        save();
        reply.result(state());
    });

    methods.on("rename", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        fence_.name = args.at(0).get<std::string>();
        setTitle(fence_.name);
        save();
        reply.result(state());
    });

    methods.on("setTitleHeight", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        fence_.titleHeight = validTitleHeight(args.at(0).get<int>());
        if (minified_) {
            resizeHeight(fence_.titleHeight);
        }
        save();
        reply.result(state());
    });

    methods.on("setLocked", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        fence_.locked = args.at(0).get<bool>();
        save();
        reply.result(state());
    });

    methods.on("setMinify", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        fence_.canMinify = args.at(0).get<bool>();
        if (fence_.canMinify) {
            minify();
        } else {
            expand();
        }
        save();
        reply.result(state());
    });

    // The page asks these as the pointer leaves or enters the fence. Only the fence's own setting decides.
    methods.on("minify", [this](WebViewWindow&, const Json&, const Reply& reply) {
        minify();
        reply.result(Json{{"minified", minified_}});
    });

    methods.on("expand", [this](WebViewWindow&, const Json&, const Reply& reply) {
        expand();
        reply.result(Json{{"minified", minified_}});
    });

    methods.on("beginMove", [this](WebViewWindow&, const Json&, const Reply& reply) {
        if (!fence_.locked) {
            beginMove();
        }
        reply.result(nullptr);
    });

    methods.on("beginResize", [this](WebViewWindow&, const Json& args, const Reply& reply) {
        const int edge = args.at(0).get<int>();
        const bool topEdge = minified_ && std::find(std::begin(kTopEdges), std::end(kTopEdges), edge) != std::end(kTopEdges);
        if (!fence_.locked && !topEdge && edge >= 0 && edge <= 7) {
            beginResize(static_cast<ResizeEdge>(edge));
        }
        reply.result(nullptr);
    });

    methods.on("newFence", [this](WebViewWindow&, const Json&, const Reply& reply) {
        app_.createFence("New fence");
        reply.result(nullptr);
    });

    methods.on("removeFence", [this](WebViewWindow&, const Json&, const Reply& reply) {
        removeFence();
        reply.result(nullptr);
    });
}

Json FenceWindow::state() {
    folderWatch_->setFolders(foldersOf(fence_.files));
    Json items = Json::array();
    for (const std::string& entry : fence_.files) {
        if (!entryExists(entry)) {
            continue;
        }
        items.push_back(Json{
            {"path", entry},
            {"label", entryLabel(entry)},
            {"link", isLink(entry)},
            {"icon", iconFor(entry)},
        });
    }
    return Json{
        {"name", fence_.name},
        {"locked", fence_.locked},
        {"canMinify", fence_.canMinify},
        {"minified", minified_},
        {"titleHeight", fence_.titleHeight},
        {"transparent", transparentBackgroundSupported()},
        {"items", items},
    };
}

void FenceWindow::onClosed() {
    app_.windowClosed(this);
}

// Keeps the fence's place and size current, so the next start opens it where it was left.
void FenceWindow::onFrameChanged() {
    if (removed_) {
        return;
    }
    if (const auto rect = frame()) {
        fence_.posX = rect->x;
        fence_.posY = rect->y;
    }
    if (const auto size = frameUnscaled()) {
        fence_.width = size->first;
        if (!minified_) {
            fence_.height = size->second;
        }
    }
    saveFrame();
}

void FenceWindow::save() {
    if (removed_) {
        return;
    }
    app_.store().save(fence_);
}

// Moving a window saves it many times over, so a failure is reported once, until a save works again.
void FenceWindow::saveFrame() {
    if (removed_) {
        return;
    }
    try {
        app_.store().save(fence_);
        saveFailed_ = false;
    } catch (const std::exception& problem) {
        if (!saveFailed_) {
            saveFailed_ = true;
            showError(std::string("NoFences couldn't save this fence's position and size.\n") + problem.what());
        }
    }
}

void FenceWindow::minify() {
    if (!fence_.canMinify || minified_) {
        return;
    }
    const auto size = frameUnscaled();
    if (!size) {
        return;
    }
    expandedHeight_ = size->second;
    minified_ = true;
    resize(size->first, fence_.titleHeight);
}

void FenceWindow::expand() {
    if (!minified_) {
        return;
    }
    minified_ = false;
    if (const auto size = frameUnscaled()) {
        resize(size->first, expandedHeight_);
    }
}

void FenceWindow::resizeHeight(int height) {
    if (const auto size = frameUnscaled()) {
        resize(size->first, height);
    }
}

// The fence is only marked removed once its folder is gone, so a failed delete leaves the window working. It closes after
// the page's request has been answered, since the window is deleted only once the event loop is past this call.
void FenceWindow::removeFence() {
    app_.store().remove(fence_);
    removed_ = true;
    runOnUiThread([this] { close(); });
}

std::string FenceWindow::iconFor(const std::string& entry) {
    const std::int64_t stamp = entryWriteStamp(entry);
    auto cached = icons_.find(entry);
    if (cached != icons_.end() && cached->second.stamp == stamp) {
        return cached->second.url;
    }
    std::string url;
    if (const auto png = entryIconPng(entry)) {
        url = "data:image/png;base64," + base64Encode(*png);
    }
    icons_[entry] = {stamp, url};
    return url;
}

void FenceWindow::onFolderChanged() {
    if (removed_) {
        return;
    }
    runScript("if (window.refreshFromHost) { window.refreshFromHost(); }");
}

}  // namespace nofences
