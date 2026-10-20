// Calls a function on the UI thread when something in a watched folder is written, renamed or removed. Each folder is watched
// for as long as it's in the set given to setFolders.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>


class DirectoryWatch {
public:
    // onChange runs on the UI thread. Changes that arrive together are reported once.
    explicit DirectoryWatch(std::function<void()> onChange);
    ~DirectoryWatch();
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;

    // Watches exactly these folders from now on. A folder that's already watched keeps its watch.
    void setFolders(const std::set<std::string>& folders);

private:
    // What a watch reports to. A posted report checks alive before calling onChange, so a report that arrives after the watch
    // is gone is dropped.
    struct Shared {
        std::function<void()> onChange;
        std::atomic<bool> alive{true};
        std::atomic<bool> pending{false};
    };

    class Folder;

    std::shared_ptr<Shared> shared_;
    std::map<std::string, std::unique_ptr<Folder>> folders_;
};

