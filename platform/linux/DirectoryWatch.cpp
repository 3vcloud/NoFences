// Linux: folder changes aren't reported yet, so the page refreshes its entries only when it changes one itself. The watch keeps
// the folder set, so a file-monitor implementation can take over here without changing the callers.

#include "DirectoryWatch.h"

#include <utility>


class DirectoryWatch::Folder {
public:
    Folder(const std::string&, std::shared_ptr<Shared>) {}
};

DirectoryWatch::DirectoryWatch(std::function<void()> onChange) : shared_(std::make_shared<Shared>()) {
    shared_->onChange = std::move(onChange);
}

DirectoryWatch::~DirectoryWatch() {
    shared_->alive = false;
    folders_.clear();
}

void DirectoryWatch::setFolders(const std::set<std::string>& folders) {
    for (auto watched = folders_.begin(); watched != folders_.end();) {
        if (folders.count(watched->first) == 0) {
            watched = folders_.erase(watched);
        } else {
            ++watched;
        }
    }
    for (const std::string& folder : folders) {
        if (folders_.count(folder) == 0) {
            folders_.emplace(folder, std::make_unique<Folder>(folder, shared_));
        }
    }
}

