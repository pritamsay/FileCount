#pragma once
#include "count_worker.h"

static bool sortedByFileCount(IFolderView2* view) {
    SORTCOLUMN primary = {};
    return SUCCEEDED(view->GetSortColumns(&primary, 1)) &&
           IsEqualPropertyKey(primary.propkey, PKEY_FileCount);
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

struct CountRow {
    std::shared_ptr<ITEMIDLIST> id;
    wstring path, name;
};

struct ViewCounts {
    ULONGLONG lastUpdate = 0, lastProbe = 0;
    wstring folder;
    bool icons = false, hidden = false, protectedFiles = false, changed = false;
    bool enumerating = false, publishing = false;
    DWORD cursor = 0, total = 0;
    std::map<wstring, CachedCount> cached;
    std::map<wstring, ULONGLONG> iconCounts;
    std::map<wstring, unsigned> nameUses;
    std::vector<CountRow> rows;
    ComPtr<IShellItemArray> items;
    std::shared_ptr<ITEMIDLIST> sample;
    std::shared_ptr<CountScan> scan;

    ~ViewCounts() { cancel(); }
    void cancel() { if (scan) scan->cancelled = true; scan.reset(); }
    bool busy() const { return enumerating || publishing || bool(scan); }
    void reset() {
        cancel();
        items.Reset(); rows.clear(); sample.reset(); nameUses.clear(); iconCounts.clear();
        enumerating = publishing = changed = false;
        lastUpdate = lastProbe = 0;
    }
};

static bool viewHasCachedCount(IFolderView2* view, const std::shared_ptr<ITEMIDLIST>& sample) {
    if (!sample) return true; // Empty/unreadable folders must not trigger a hot retry loop.
    PROPVARIANT value = {};
    HRESULT hr = view->GetViewProperty(ILFindLastID(sample.get()), PKEY_FileCount, &value);
    bool present = SUCCEEDED(hr) && value.vt == VT_UI8;
    PropVariantClear(&value);
    return present;
}

static void resortByCount(IFolderView2* view) {
    int number = 0;
    if (FAILED(view->GetSortColumnCount(&number)) || number < 1 || number > 16) return;
    std::vector<SORTCOLUMN> sort(number);
    if (FAILED(view->GetSortColumns(sort.data(), number)) ||
        !IsEqualPropertyKey(sort.front().propkey, PKEY_FileCount)) return;
    // Keep the primary sort and its direction. Resort once after the whole batch.
    if (sort.size() >= 3 && IsEqualPropertyKey(sort.back().propkey, PKEY_ItemPathDisplay)) {
        sort.back().direction = sort.back().direction == SORT_ASCENDING ? SORT_DESCENDING : SORT_ASCENDING;
    } else {
        sort.push_back({PKEY_ItemNameDisplay, SORT_ASCENDING});
        sort.push_back({PKEY_ItemPathDisplay, SORT_ASCENDING});
    }
    view->SetSortColumns(sort.data(), static_cast<int>(sort.size()));
}

// Only bounded shell work happens here. All filesystem I/O belongs to CountWorkers.
static HRESULT updateCounts(IFolderView2* view, bool icons, ViewCounts& state, CountWorkers& workers) {
    ComPtr<IPersistFolder2> folder;
    HRESULT hr = view->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr;
    PIDLIST_ABSOLUTE location = nullptr;
    if (FAILED(hr = folder->GetCurFolder(&location))) return hr;
    PWSTR name = nullptr;
    hr = SHGetNameFromIDList(location, SIGDN_FILESYSPATH, &name);
    CoTaskMemFree(location);
    if (FAILED(hr)) return hr;
    wstring parent(name);
    CoTaskMemFree(name);
    SHELLSTATE options = {};
    SHGetSetSettings(&options, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    bool settingsChanged = state.hidden != bool(options.fShowAllObjects) ||
                           state.protectedFiles != bool(options.fShowSuperHidden);
    if (settingsChanged || state.folder != parent) {
        state.reset(); state.cached.clear(); state.folder = parent;
        state.hidden = options.fShowAllObjects;
        state.protectedFiles = options.fShowSuperHidden;
    }
    if (state.icons != icons) { state.reset(); state.icons = icons; }
    auto now = GetTickCount64();
    if (!state.busy()) {
        if (state.lastUpdate && now - state.lastProbe < 1500) return S_OK;
        state.lastProbe = now;
        int currentTotal = 0;
        view->ItemCount(SVGIO_ALLVIEW, &currentTotal);
        bool displayValid = currentTotal == static_cast<int>(state.total) && viewHasCachedCount(view, state.sample);
        if (state.lastUpdate && now - state.lastUpdate < 30000 &&
            displayValid) return S_OK;
        // F5 can clear the view properties. Reuse disk counts where still valid.
        auto previousIcons = displayValid ? std::move(state.iconCounts) : std::map<wstring, ULONGLONG>{};
        auto previousUpdate = displayValid ? state.lastUpdate : 0;
        state.reset(); state.lastProbe = now;
        state.iconCounts = std::move(previousIcons); state.lastUpdate = previousUpdate;
        if (FAILED(hr = view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&state.items)))) return hr;
        if (FAILED(hr = state.items->GetCount(&state.total))) return hr;
        state.cursor = 0; state.enumerating = true;
    }
    auto deadline = GetTickCount64() + 12;
    if (state.enumerating) {
        unsigned batch = 0;
        while (state.cursor < state.total && batch++ < 32 && GetTickCount64() < deadline) {
            ComPtr<IShellItem> item;
            if (FAILED(state.items->GetItemAt(state.cursor++, &item))) continue;
            wstring display;
            PWSTR text = nullptr;
            if (icons && SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text))) {
                display = text; CoTaskMemFree(text); ++state.nameUses[display];
            }
            SFGAOF attributes = 0;
            if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes)) ||
                attributes != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) continue;
            PWSTR path = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) continue;
            CountRow row; row.path = path; row.name = display;
            CoTaskMemFree(path);
            PIDLIST_ABSOLUTE id = nullptr;
            if (FAILED(SHGetIDListFromObject(item.Get(), &id))) continue;
            row.id = std::shared_ptr<ITEMIDLIST>(const_cast<ITEMIDLIST*>(id), [](ITEMIDLIST* p) { CoTaskMemFree(p); });
            state.rows.push_back(std::move(row));
        }
        if (state.cursor < state.total) return state.iconCounts.empty() ? S_FALSE : S_OK;
        state.items.Reset(); state.enumerating = false;
        state.scan = std::make_shared<CountScan>();
        state.scan->hidden = state.hidden; state.scan->protectedFiles = state.protectedFiles;
        for (const auto& row : state.rows) {
            state.scan->paths.push_back(row.path);
            auto cached = state.cached.find(row.path);
            if (cached != state.cached.end()) state.scan->counts.emplace(*cached);
        }
        if (!workers.submit(state.scan)) { state.cancel(); state.lastUpdate = now; }
        return state.iconCounts.empty() ? S_FALSE : S_OK;
    }
    if (state.scan) {
        if (!state.scan->done.load(std::memory_order_acquire)) return state.iconCounts.empty() ? S_FALSE : S_OK;
        state.cached = std::move(state.scan->counts); state.scan.reset();
        state.publishing = true; state.cursor = 0;
    }
    if (state.publishing) {
        unsigned batch = 0;
        while (state.cursor < state.rows.size() && batch++ < 32 && GetTickCount64() < deadline) {
            auto& row = state.rows[state.cursor++];
            ULONGLONG files = state.cached[row.path].files;
            PROPVARIANT old = {}, value = {};
            auto id = ILFindLastID(row.id.get());
            view->GetViewProperty(id, PKEY_FileCount, &old);
            if (files != UnknownCount) { value.vt = VT_UI8; value.uhVal.QuadPart = files; }
            if (old.vt != value.vt || (value.vt == VT_UI8 && old.uhVal.QuadPart != files))
                if (SUCCEEDED(view->SetViewProperty(id, PKEY_FileCount, value))) state.changed = true;
            if (!state.sample && files != UnknownCount) state.sample = row.id;
            PropVariantClear(&old);
        }
        if (state.cursor < state.rows.size()) return state.iconCounts.empty() ? S_FALSE : S_OK;
        state.iconCounts.clear();
        for (const auto& row : state.rows)
            if (icons && state.nameUses[row.name] == 1) state.iconCounts[row.name] = state.cached[row.path].files;
        state.publishing = false; state.lastUpdate = GetTickCount64();
        if (state.changed) resortByCount(view);
        state.changed = false;
    }
    return S_OK;
}
