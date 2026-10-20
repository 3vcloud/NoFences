#include "FenceWindow.h"

#include <algorithm>
#include <iterator>
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

gwl::WindowOptions windowOptions(const Fence& fence) {
    gwl::WindowOptions options;
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

}  // namespace

FenceWindow::FenceWindow(FenceApp& app, Fence fence)
    : WebViewWindow(windowOptions(fence)),
      app_(app),
      fence_(std::move(fence)),
      minified_(fence_.canMinify),
      expandedHeight_(fence_.height) {
    registerMethods();
}

std::string FenceWindow::html() const {
    return embeddedPage();
}

void FenceWindow::registerMethods() {
    auto& methods = handler();

    methods.on("state", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) { reply.result(state()); });

    methods.on("open", [](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        openTarget(args.at(0).get<std::string>());
        reply.result(nullptr);
    });

    methods.on("showInFolder", [](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        openTarget(pathToUtf8(pathFromUtf8(args.at(0).get<std::string>()).parent_path()));
        reply.result(nullptr);
    });

    methods.on("openDesktop", [](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        openTarget(pathToUtf8(desktopDirectory()));
        reply.result(nullptr);
    });

    methods.on("openConfig", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        openTarget(pathToUtf8(app_.store().metadataPath(fence_)));
        reply.result(nullptr);
    });

    // Takes what the page was dropped: file addresses from the file manager, or links from the browser.
    // Says in problems what wasn't added, and why, so a drop that does nothing still tells the user.
    methods.on("addEntries", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        bool added = false;
        gwl::Json problems = gwl::Json::array();
        for (const gwl::Json& uri : args.at(0)) {
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
        gwl::Json result = state();
        result["problems"] = problems;
        reply.result(result);
    });

    methods.on("addLink",[this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
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

    methods.on("remove", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        const std::string entry = args.at(0).get<std::string>();
        fence_.files.erase(std::remove(fence_.files.begin(), fence_.files.end(), entry), fence_.files.end());
        save();
        reply.result(state());
    });

    methods.on("rename", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        fence_.name = args.at(0).get<std::string>();
        setTitle(fence_.name);
        save();
        reply.result(state());
    });

    methods.on("setTitleHeight", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        fence_.titleHeight = validTitleHeight(args.at(0).get<int>());
        if (minified_) {
            resizeHeight(fence_.titleHeight);
        }
        save();
        reply.result(state());
    });

    methods.on("setLocked", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        fence_.locked = args.at(0).get<bool>();
        save();
        reply.result(state());
    });

    methods.on("setMinify", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
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
    methods.on("minify", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        minify();
        reply.result(gwl::Json{{"minified", minified_}});
    });

    methods.on("expand", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        expand();
        reply.result(gwl::Json{{"minified", minified_}});
    });

    methods.on("beginMove", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        if (!fence_.locked) {
            beginMove();
        }
        reply.result(nullptr);
    });

    methods.on("beginResize", [this](gwl::WebViewWindow&, const gwl::Json& args, const gwl::Reply& reply) {
        const int edge = args.at(0).get<int>();
        const bool topEdge = minified_ && std::find(std::begin(kTopEdges), std::end(kTopEdges), edge) != std::end(kTopEdges);
        if (!fence_.locked && !topEdge && edge >= 0 && edge <= 7) {
            beginResize(static_cast<gwl::ResizeEdge>(edge));
        }
        reply.result(nullptr);
    });

    methods.on("newFence", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        app_.createFence("New fence");
        reply.result(nullptr);
    });

    methods.on("removeFence", [this](gwl::WebViewWindow&, const gwl::Json&, const gwl::Reply& reply) {
        removeFence();
        reply.result(nullptr);
    });
}

gwl::Json FenceWindow::state() {
    gwl::Json items = gwl::Json::array();
    for (const std::string& entry : fence_.files) {
        if (!entryExists(entry)) {
            continue;
        }
        items.push_back(gwl::Json{
            {"path", entry},
            {"label", entryLabel(entry)},
            {"link", isLink(entry)},
            {"icon", iconFor(entry)},
        });
    }
    return gwl::Json{
        {"name", fence_.name},
        {"locked", fence_.locked},
        {"canMinify", fence_.canMinify},
        {"minified", minified_},
        {"titleHeight", fence_.titleHeight},
        {"transparent", gwl::transparentBackgroundSupported()},
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
            gwl::showError(std::string("NoFences couldn't save this fence's position and size.\n") + problem.what());
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
    gwl::runOnUiThread([this] { close(); });
}

std::string FenceWindow::iconFor(const std::string& entry) {
    auto cached = icons_.find(entry);
    if (cached != icons_.end()) {
        return cached->second;
    }
    std::string url;
    if (const auto png = entryIconPng(entry)) {
        url = "data:image/png;base64," + base64Encode(*png);
    }
    icons_[entry] = url;
    return url;
}

}  // namespace nofences
