#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
namespace fs = std::filesystem;

struct CachedCount {
    FILETIME modified = {};
    ULONGLONG files = UnknownCount;
    ULONGLONG checked = 0;
};

// This code runs only in our worker process threads, never in Explorer.
// A timeout or cancellation returns unknown, not a misleading partial total.
static ULONGLONG countDirect(const fs::path& path, bool hidden, bool protectedFiles,
                            const std::atomic<bool>& cancelled, ULONGLONG deadline) {
    if (cancelled || GetTickCount64() >= deadline) return UnknownCount;
    WIN32_FIND_DATAW entry;
    wstring pattern = (path / L"*").wstring();
    if (pattern.rfind(L"\\\\?\\", 0) != 0) {
        if (pattern.rfind(L"\\\\", 0) == 0) pattern = L"\\\\?\\UNC\\" + pattern.substr(2);
        else pattern = L"\\\\?\\" + pattern;
    }
    HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : UnknownCount;
    ULONGLONG count = 0;
    DWORD last = ERROR_SUCCESS;
    do {
        if (cancelled || GetTickCount64() >= deadline) break;
        DWORD flags = entry.dwFileAttributes;
        if (!(flags & FILE_ATTRIBUTE_DIRECTORY) &&
            _wcsicmp(entry.cFileName, L"desktop.ini") && _wcsicmp(entry.cFileName, L"Thumbs.db") &&
            (!(flags & FILE_ATTRIBUTE_HIDDEN) || (hidden && (!(flags & FILE_ATTRIBUTE_SYSTEM) || protectedFiles))))
            ++count;
        if (!FindNextFileW(search, &entry)) { last = GetLastError(); break; }
    } while (true);
    FindClose(search);
    return last == ERROR_NO_MORE_FILES && !cancelled ? count : UnknownCount;
}

struct CountScan {
    std::atomic<bool> cancelled{false}, done{false};
    bool hidden = false, protectedFiles = false;
    std::vector<wstring> paths;
    std::map<wstring, CachedCount> counts;
};

class CountWorkers {
public:
    using Counter = decltype(&countDirect);
private:
    struct Queue {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::shared_ptr<CountScan>> pending;
        std::vector<std::weak_ptr<CountScan>> active;
        bool stopping = false;
    };
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();

    static void work(std::shared_ptr<Queue> queue, Counter counter) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::shared_ptr<CountScan> scan;
            {
                std::unique_lock<std::mutex> lock(queue->mutex);
                queue->wake.wait(lock, [&] { return queue->stopping || !queue->pending.empty(); });
                if (queue->stopping) return;
                scan = queue->pending.front();
                queue->pending.pop_front();
                queue->active.erase(std::remove_if(queue->active.begin(), queue->active.end(),
                    [](const auto& item) { return item.expired(); }), queue->active.end());
                queue->active.push_back(scan);
            }
            auto deadline = GetTickCount64() + 30000;
            for (const auto& path : scan->paths) {
                if (scan->cancelled) break;
                auto& count = scan->counts[path];
                auto now = GetTickCount64();
                if (now >= deadline) { count.files = UnknownCount; continue; }
                WIN32_FILE_ATTRIBUTE_DATA info = {};
                if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
                    count.files = UnknownCount;
                    count.checked = now;
                    continue;
                }
                if (!count.checked || CompareFileTime(&count.modified, &info.ftLastWriteTime) ||
                    now - count.checked >= 60000) {
                    count.files = counter(path, scan->hidden, scan->protectedFiles,
                        scan->cancelled, (std::min)(deadline, now + 3000));
                    count.modified = info.ftLastWriteTime;
                    count.checked = now;
                }
            }
            scan->done.store(true, std::memory_order_release);
        }
    }
public:
    explicit CountWorkers(Counter counter = countDirect) {
        // Two bounded workers: an unavailable network share cannot block the UI.
        // They own their state, so shutdown never waits for a network I/O call.
        for (int i = 0; i < 2; ++i) std::thread(work, queue, counter).detach();
    }
    ~CountWorkers() {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->stopping = true;
        for (auto& scan : queue->pending) scan->cancelled = true;
        for (auto& weak : queue->active) if (auto scan = weak.lock()) scan->cancelled = true;
        queue->wake.notify_all();
    }
    bool submit(const std::shared_ptr<CountScan>& scan) {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->pending.erase(std::remove_if(queue->pending.begin(), queue->pending.end(),
            [](const auto& item) { return bool(item->cancelled); }), queue->pending.end());
        if (queue->pending.size() >= 16) return false;
        queue->pending.push_back(scan);
        queue->wake.notify_one();
        return true;
    }
};
