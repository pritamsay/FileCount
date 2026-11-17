#pragma once
#include <filesystem>
#include <map>
namespace fs = std::filesystem;
static ULONGLONG countDirect(const fs::path& path, bool showHidden, bool showProtected) {
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
    do {
        DWORD flags = entry.dwFileAttributes;
        if (flags & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(entry.cFileName, L"desktop.ini") || !_wcsicmp(entry.cFileName, L"Thumbs.db"))
            continue;
        if (flags & FILE_ATTRIBUTE_HIDDEN) {
            if (!showHidden || ((flags & FILE_ATTRIBUTE_SYSTEM) && !showProtected)) continue;
        }
        ++count;
    } while (FindNextFileW(search, &entry));
    DWORD last = GetLastError();
    FindClose(search);
    return last == ERROR_NO_MORE_FILES ? count : UnknownCount;
}

static bool sortedByFileCount(IFolderView2* view) {
    int number = 0;
    if (FAILED(view->GetSortColumnCount(&number)) || number < 1) return false;
    std::vector<SORTCOLUMN> order(number);
    return SUCCEEDED(view->GetSortColumns(order.data(), number)) &&
           IsEqualPropertyKey(order.front().propkey, PKEY_FileCount);
}

static bool fileCountColumnVisible(IShellView* shell) {
    ComPtr<IColumnManager> columns;
    UINT size = 0;
    if (FAILED(shell->QueryInterface(IID_PPV_ARGS(&columns))) ||
        FAILED(columns->GetColumnCount(CM_ENUM_VISIBLE, &size)) || !size) return false;
    std::vector<PROPERTYKEY> keys(size);
    if (FAILED(columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), size))) return false;
    for (const auto& key : keys)
        if (IsEqualPropertyKey(key, PKEY_FileCount)) return true;
    return false;
}

struct CachedCount {
    FILETIME modified = {};
    ULONGLONG files = UnknownCount;
    ULONGLONG checked = 0;
};

struct ViewCounts {
    ULONGLONG lastUpdate = 0;
    wstring folder;
    bool icons = false;
    bool hidden = false;
    bool protectedFiles = false;
    std::map<wstring, CachedCount> cached;
    std::map<wstring, ULONGLONG> iconCounts;
    ComPtr<IShellItem> sample;
};

static bool viewHasCachedCount(IFolderView2* view, IShellItem* sample) {
    if (!sample) return false;
    PIDLIST_ABSOLUTE id = nullptr;
    if (FAILED(SHGetIDListFromObject(sample, &id))) return false;
    PROPVARIANT value = {};
    HRESULT hr = view->GetViewProperty(ILFindLastID(id), PKEY_FileCount, &value);
    CoTaskMemFree(id);
    bool present = SUCCEEDED(hr) && value.vt == VT_UI8;
    PropVariantClear(&value);
    return present;
}

// Each Explorer window has its own folder, sort order and display cache.
static HRESULT updateCounts(IFolderView2* view, bool icons, ViewCounts& state) {
    ComPtr<IPersistFolder2> folder;
    HRESULT hr = view->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE location = nullptr;
    hr = folder->GetCurFolder(&location);
    if (FAILED(hr)) return hr;
    PWSTR name = nullptr;
    hr = SHGetNameFromIDList(location, SIGDN_FILESYSPATH, &name);
    CoTaskMemFree(location);
    if (FAILED(hr)) return hr;
    fs::path parent(name);
    CoTaskMemFree(name);
    DWORD flags = GetFileAttributesW(parent.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY)) return S_FALSE;
    ULONGLONG now = GetTickCount64();
    SHELLSTATE options = {};
    SHGetSetSettings(&options, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    bool settingsChanged = state.hidden != bool(options.fShowAllObjects) ||
                           state.protectedFiles != bool(options.fShowSuperHidden);
    if (!settingsChanged && icons == state.icons && state.folder == parent.native() &&
        state.lastUpdate && now - state.lastUpdate < 4000 &&
        viewHasCachedCount(view, state.sample.Get())) return S_OK;
    if (settingsChanged || state.folder != parent.native()) state.cached.clear();
    state.hidden = options.fShowAllObjects;
    state.protectedFiles = options.fShowSuperHidden;
    state.folder = parent.native();
    state.lastUpdate = now;
    state.icons = icons;
    state.iconCounts.clear();
    state.sample.Reset();

    ComPtr<IShellItemArray> items;
    hr = view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
    if (FAILED(hr)) return hr;
    DWORD total = 0;
    items->GetCount(&total);
    bool changed = false;
    for (DWORD i = 0; i < total; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        // A folder and a file can have the same displayed name when extensions
        // are hidden. Suppress ambiguous badges instead of marking the file.
        bool unique = false;
        wstring displayName;
        if (icons) {
            PWSTR text = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text))) {
                displayName = text;
                CoTaskMemFree(text);
                auto inserted = state.iconCounts.emplace(displayName, UnknownCount);
                unique = inserted.second;
                if (!unique) inserted.first->second = UnknownCount;
            }
        }
        SFGAOF attributes = 0;
        if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes)) ||
            attributes != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) continue;
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) continue;
        fs::path child(path);
        CoTaskMemFree(path);
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(child.c_str(), GetFileExInfoStandard, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        auto& count = state.cached[child.native()];
        if (!count.checked || CompareFileTime(&count.modified, &info.ftLastWriteTime) ||
                now - count.checked > 60000) {
            count.files = countDirect(child, state.hidden, state.protectedFiles);
            count.modified = info.ftLastWriteTime;
            count.checked = now;
        }
        if (unique) state.iconCounts[displayName] = count.files;
        if (!state.sample && count.files != UnknownCount) state.sample = item;
        PIDLIST_ABSOLUTE id = nullptr;
        if (FAILED(SHGetIDListFromObject(item.Get(), &id))) continue;
        auto row = ILFindLastID(id);
        PROPVARIANT old = {};
        view->GetViewProperty(row, PKEY_FileCount, &old);
        PROPVARIANT value = {};
        if (count.files != UnknownCount) {
            value.vt = VT_UI8;
            value.uhVal.QuadPart = count.files;
        }
        if (old.vt != value.vt || (value.vt == VT_UI8 && old.uhVal.QuadPart != count.files)) {
            if (SUCCEEDED(view->SetViewProperty(row, PKEY_FileCount, value))) changed = true;
        }
        PropVariantClear(&old);
        CoTaskMemFree(id);
    }
    int sortCount = 0;
    if (changed && SUCCEEDED(view->GetSortColumnCount(&sortCount)) && sortCount > 0) {
        std::vector<SORTCOLUMN> sort(sortCount);
        if (SUCCEEDED(view->GetSortColumns(sort.data(), sortCount)) &&
            IsEqualPropertyKey(sort.front().propkey, PKEY_FileCount)) {
            // A changed trailing sort key makes Explorer re-evaluate its cache.
            // Keep File count first throughout, including after F5.
            if (sort.size() >= 3 && IsEqualPropertyKey(sort.back().propkey, PKEY_ItemPathDisplay)) {
                sort.back().direction = sort.back().direction == SORT_ASCENDING ?
                                        SORT_DESCENDING : SORT_ASCENDING;
            } else {
                sort.push_back({PKEY_ItemNameDisplay, SORT_ASCENDING});
                sort.push_back({PKEY_ItemPathDisplay, SORT_ASCENDING});
            }
            view->SetSortColumns(sort.data(), static_cast<int>(sort.size()));
        }
    }
    return S_OK;
}
