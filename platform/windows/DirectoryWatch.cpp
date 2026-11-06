// Windows: each watched folder has a thread that waits for the folder's changes with ReadDirectoryChangesW.

#include "DirectoryWatch.h"

#include <windows.h>

#include <memory>
#include <utility>
#include <vector>

#include "WebViewWindow.h"


// The changes that alter an entry's name or icon: files being created, renamed, removed or written.
const DWORD kWatchedChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

// One folder's watch: a thread that reports the folder's changes, and an event that stops it.
class DirectoryWatch::Folder {
public:
    Folder(const std::string& path, std::shared_ptr<Shared> shared) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // The thread takes ownership of its context, so the context outlives this object's reference to it.
        auto* context = new Context{toWide(path), std::move(shared), stop_};
        thread_ = CreateThread(nullptr, 0, &Folder::run, context, 0, nullptr);
        if (!thread_) {
            delete context;
        }
    }

    ~Folder() {
        SetEvent(stop_);
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        CloseHandle(stop_);
    }

    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

private:
    struct Context {
        std::wstring path;
        std::shared_ptr<Shared> shared;
        HANDLE stop;
    };

    static std::wstring toWide(const std::string& text) {
        if (text.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    }

    // Tells the UI thread the folder changed. Reports that arrive before it runs are folded into that one call.
    static void report(const std::shared_ptr<Shared>& shared) {
        if (shared->pending.exchange(true)) {
            return;
        }
        runOnUiThread([shared] {
            shared->pending = false;
            if (shared->alive && shared->onChange) {
                shared->onChange();
            }
        });
    }

    static DWORD WINAPI run(LPVOID parameter) {
        std::unique_ptr<Context> context(static_cast<Context*>(parameter));
        HANDLE folder = CreateFileW(context->path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (folder == INVALID_HANDLE_VALUE) {
            return 0;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<BYTE> changes(64 * 1024);
        while (true) {
            ResetEvent(overlapped.hEvent);
            if (!ReadDirectoryChangesW(folder, changes.data(), static_cast<DWORD>(changes.size()), FALSE, kWatchedChanges, nullptr,
                                       &overlapped, nullptr)) {
                break;
            }
            const HANDLE waits[] = {context->stop, overlapped.hEvent};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                break;
            }
            DWORD transferred = 0;
            GetOverlappedResult(folder, &overlapped, &transferred, FALSE);
            report(context->shared);
        }
        // A read that's still pending is cancelled and waited for, so nothing uses its buffer or event after this.
        CancelIo(folder);
        DWORD ignored = 0;
        GetOverlappedResult(folder, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        CloseHandle(folder);
        return 0;
    }

    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
};

DirectoryWatch::DirectoryWatch(std::function<void()> onChange) : shared_(std::make_shared<Shared>()) {
    shared_->onChange = std::move(onChange);
}

DirectoryWatch::~DirectoryWatch() {
    // Reports already posted check alive, so they're dropped once the watch is gone.
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

