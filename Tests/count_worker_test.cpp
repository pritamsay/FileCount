#include "../Source/common.h"
#include "../Source/count_worker.h"
#include <fstream>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
    if (!ok) ++failures;
}
static std::atomic<int> entered{0};
static std::atomic<bool> releaseScans{false};
static ULONGLONG slowCounter(const fs::path&, bool, bool,
                             const std::atomic<bool>& cancelled, ULONGLONG) {
    ++entered;
    while (!releaseScans && !cancelled) Sleep(5);
    return cancelled ? UnknownCount : 7;
}
template<class Predicate> static bool waitUntil(Predicate ready) {
    auto deadline = GetTickCount64() + 4000;
    while (!ready() && GetTickCount64() < deadline) Sleep(5);
    return ready();
}

int wmain() {
    wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
    auto root = fs::path(temp) / (L"FileCount-test-" + std::to_wstring(GetCurrentProcessId()));
    // This process owns the unique temporary fixture; no existing user folders.
    if (fs::exists(root)) return 2;
    fs::create_directories(root / L"empty");
    fs::create_directories(root / L"mixed" / L"nested");
    for (auto file : {L"visible.txt", L"hidden.txt", L"protected.txt", L"desktop.ini", L"Thumbs.db"})
        std::ofstream(root / L"mixed" / file) << "fixture";
    std::ofstream(root / L"mixed" / L"nested" / L"child.txt") << "not counted";
    SetFileAttributesW((root / L"mixed" / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
    SetFileAttributesW((root / L"mixed" / L"protected.txt").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    std::atomic<bool> cancelled{false};
    auto count = [&](const wchar_t* folder, bool hidden, bool protectedFiles) {
        return countDirect(root / folder, hidden, protectedFiles, cancelled, GetTickCount64() + 3000);
    };
    check(count(L"empty", false, false) == 0, "empty folder is zero");
    check(count(L"mixed", false, false) == 1, "direct visible files only; metadata excluded");
    check(count(L"mixed", true, false) == 2, "hidden enabled, protected excluded");
    check(count(L"mixed", true, true) == 3, "protected enabled");
    check(count(L"missing", false, false) == UnknownCount, "missing folder is unknown");
    cancelled = true;
    check(count(L"mixed", true, true) == UnknownCount, "cancellation never returns partial total");
    cancelled = false;
    check(countDirect(root, true, true, cancelled, GetTickCount64()) == UnknownCount, "expired deadline is unknown");
    {
        CountWorkers workers(slowCounter);
        std::vector<std::shared_ptr<CountScan>> jobs;
        auto began = GetTickCount64();
        for (int i = 0; i < 3; ++i) {
            auto job = std::make_shared<CountScan>(); job->paths.push_back(root.native());
            check(workers.submit(job), "background job accepted"); jobs.push_back(job);
        }
        check(GetTickCount64() - began < 100, "submission does not wait for slow I/O");
        check(waitUntil([] { return entered == 2; }), "only two workers run concurrently");
        jobs[0]->cancelled = true;
        check(waitUntil([&] { return jobs[0]->done.load(); }), "obsolete job cancelled");
        check(waitUntil([] { return entered == 3; }), "queued job starts after cancelled job");
        releaseScans = true;
        check(waitUntil([&] { return jobs[1]->done.load() && jobs[2]->done.load(); }), "queued work completes");
        check(jobs[2]->counts[root.native()].files == 7, "result publication after completion");
    }
    SetFileAttributesW((root / L"mixed" / L"protected.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW((root / L"mixed" / L"hidden.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
    if (fs::canonical(root).parent_path() != fs::canonical(fs::path(temp))) return 2;
    fs::remove_all(root);
    return failures ? 1 : 0;
}
