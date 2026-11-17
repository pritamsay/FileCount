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
};
static std::map<HWND, ExplorerPane> views;

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
        ComPtr<IShellView> shell;
        ComPtr<IFolderView2> view;
        if (FAILED(windows->Item(index, &entry)) || !entry || FAILED(entry.As(&services)) ||
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
        if (exiting || !IsWindowVisible(pane) || IsIconic(owner)) {
            state.badges.hide();
            continue;
        }
        FOLDERVIEWMODE mode = FVM_AUTO;
        int size = 0;
        if (FAILED(view->GetViewModeAndIconSize(&mode, &size))) continue;
        bool details = mode == FVM_DETAILS;
        bool iconMode = mode == FVM_ICON || mode == FVM_SMALLICON ||
                        mode == FVM_THUMBNAIL || mode == FVM_THUMBSTRIP;
        bool byCount = sortedByFileCount(view.Get());
        bool badges = iconMode && byCount;
        if (byCount || (details && fileCountColumnVisible(shell.Get()))) {
            if (updateCounts(view.Get(), badges, state.counts) == S_OK && badges) {
                state.badges.display(owner, shell.Get(), size, state.counts.iconCounts);
                continue;
            }
        } else {
            state.counts.lastUpdate = 0;
        }
        state.badges.hide();
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
        SetTimer(window, 1, 350, nullptr);
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
        updateExplorer(true);
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
