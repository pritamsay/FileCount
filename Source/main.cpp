#include "common.h"
#include <exdisp.h>
#include <propkey.h>
#include <propvarutil.h>
#include <set>
#include "livecounts.h"
#include "badges.h"
#include "cleanup.h"

// All Explorer access goes through the shell's public COM interfaces.
struct ExplorerPane {
    ViewCounts counts;
    IconBadges badges;
    HWND owner = nullptr;
    ULONGLONG retryAfter = 0;
};
static std::map<HWND, ExplorerPane> views;
static std::unique_ptr<CountWorkers> workers;
static std::unique_ptr<BadgeReader> badgeReader;
static HWINEVENTHOOK layoutEvents = nullptr;
static HWINEVENTHOOK scrollEvents = nullptr;

// Events are delivered to this process; no DLL or code is loaded into Explorer.
static void CALLBACK layoutChanged(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (event != EVENT_OBJECT_LOCATIONCHANGE && event != EVENT_OBJECT_REORDER &&
        event != EVENT_OBJECT_NAMECHANGE && event != EVENT_OBJECT_CREATE &&
        event != EVENT_OBJECT_DESTROY && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE &&
        event != EVENT_OBJECT_VALUECHANGE && event != EVENT_OBJECT_CONTENTSCROLLED &&
        event != EVENT_SYSTEM_SCROLLINGSTART && event != EVENT_SYSTEM_SCROLLINGEND) return;
    HWND owner = GetAncestor(hwnd, GA_ROOT);
    for (auto& entry : views)
        if (entry.second.owner == owner) entry.second.badges.invalidate();
}

static void updateExplorer(bool exiting = false, bool uninstall = false) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&windows)))) return;
    long total = 0;
    windows->get_Count(&total);
    std::set<HWND> open;
    for (long i = 0; i < total; ++i) {
        VARIANT index = {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> entry;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IWebBrowser2> web;
        ComPtr<IShellView> shell;
        ComPtr<IFolderView2> view;
        if (FAILED(windows->Item(index, &entry)) || !entry) continue;
        SHANDLE_PTR handle = 0;
        if (SUCCEEDED(entry.As(&web)) && SUCCEEDED(web->get_HWND(&handle)) &&
            IsHungAppWindow(reinterpret_cast<HWND>(handle))) continue;
        if (FAILED(entry.As(&services)) ||
            FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&shell)) || FAILED(shell.As(&view))) continue;

        if (uninstall) {
            cleanOpenView(shell.Get(), view.Get());
            continue;
        }
        HWND pane = nullptr;
        if (FAILED(shell->GetWindow(&pane))) continue;
        open.insert(pane);
        auto &state = views[pane];
        HWND owner = GetAncestor(pane, GA_ROOT);
        state.owner = owner;
        if (exiting || !IsWindowVisible(pane) || IsIconic(owner)) {
            state.badges.hide();
            state.counts.reset();
            continue;
        }
        if (GetTickCount64() < state.retryAfter) { state.badges.hide(); continue; }
        auto started = GetTickCount64();
        FOLDERVIEWMODE mode = FVM_AUTO;
        int size = 0;
        if (FAILED(view->GetViewModeAndIconSize(&mode, &size))) { state.badges.hide(); continue; }
        bool details = mode == FVM_DETAILS;
        bool iconMode = mode == FVM_ICON || mode == FVM_SMALLICON ||
                        mode == FVM_THUMBNAIL || mode == FVM_THUMBSTRIP;
        bool byCount = sortedByFileCount(view.Get());
        bool badges = iconMode && byCount;
        if (byCount || (details && fileCountColumnVisible(shell.Get()))) {
            auto updated = state.counts.lastUpdate;
            auto result = updateCounts(view.Get(), badges, state.counts, *workers);
            if (updated != state.counts.lastUpdate) state.badges.invalidate();
            if (result == S_OK && badges) {
                state.badges.display(owner, shell.Get(), size, state.counts.iconCounts, *badgeReader);
            } else state.badges.hide();
        } else {
            state.counts.reset();
            state.badges.hide();
        }
        // A slow provider gets a cooling-off period rather than immediate retries.
        if (GetTickCount64() - started > 150) {
            state.retryAfter = GetTickCount64() + 10000;
            state.badges.hide();
        }
    }
    for (auto it = views.begin(); it != views.end();) {
        if (!open.count(it->first)) it = views.erase(it);
        else ++it;
    }
}

static constexpr UINT trayEvent = WM_APP + 1;
static UINT taskbarCreated;
static NOTIFYICONDATAW tray = {};

static LRESULT CALLBACK messages(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &tray);
        return 0;
    }
    if (message == WM_TIMER) {
        KillTimer(window, 1);
        updateExplorer();
        bool publishing = false;
        for (const auto& pane : views)
            publishing |= pane.second.counts.enumerating || pane.second.counts.publishing;
        SetTimer(window, 1, publishing ? 150 : 750, nullptr);
        return 0;
    }
    if (message == trayEvent && (l == WM_RBUTTONUP || l == WM_LBUTTONUP)) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Explorer Counts is running");
        AppendMenuW(menu, MF_STRING, 1, L"Exit Explorer Counts");
        POINT cursor;
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x,
                                     cursor.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (choice == 1) DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, 1);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        if (layoutEvents) UnhookWinEvent(layoutEvents);
        if (scrollEvents) UnhookWinEvent(scrollEvents);
        views.clear(); // Exit never calls back into a possibly unresponsive Explorer.
        workers.reset();
        badgeReader.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static void stopInstalledHelper() {
    HWND window = FindWindowW(L"ExplorerCounts.Tray", L"Explorer Counts");
    if (!window) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    wchar_t image[32768];
    DWORD size = ARRAYSIZE(image);
    PWSTR local = nullptr;
    if (QueryFullProcessImageNameW(process, 0, image, &size) &&
        SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        auto installed = fs::path(local) / L"Programs" / L"FileCount" / L"ExplorerCounts.exe";
        if (!_wcsicmp(image, installed.c_str())) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            WaitForSingleObject(process, 5000);
        }
        CoTaskMemFree(local);
    }
    CloseHandle(process);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (!wcscmp(command, L"--stop-installed") || !wcscmp(command, L"--clean-views")) {
        stopInstalledHelper();
        if (!wcscmp(command, L"--clean-views")) {
            updateExplorer(false, true);
            cleanSavedViews(HKEY_CURRENT_USER,
                LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\Bags)");
        }
        CoUninitialize();
        return 0;
    }
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\ExplorerCounts.Tray.Instance");
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    workers = std::make_unique<CountWorkers>();
    badgeReader = std::make_unique<BadgeReader>();
    layoutEvents = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CONTENTSCROLLED, nullptr,
        layoutChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    scrollEvents = SetWinEventHook(EVENT_SYSTEM_SCROLLINGSTART, EVENT_SYSTEM_SCROLLINGEND, nullptr,
        layoutChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW type = {};
    type.hInstance = instance;
    type.lpfnWndProc = messages;
    type.lpszClassName = L"ExplorerCounts.Tray";
    RegisterClassW(&type);
    HWND window = CreateWindowW(type.lpszClassName, L"Explorer Counts", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    tray.uCallbackMessage = trayEvent;
    wcscpy_s(tray.szTip, L"Explorer Counts");
    Shell_NotifyIconW(NIM_ADD, &tray);
    SetTimer(window, 1, 500, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(single);
    CoUninitialize();
    return 0;
}
