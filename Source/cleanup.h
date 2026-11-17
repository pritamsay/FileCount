#pragma once
#include <algorithm>
#include <cstring>

// Explorer stores the column order and the sort order separately for each view.
// Only edit recognized records. Preserve widths, directions and unrelated bytes.
static DWORD readNumber(const BYTE* data) {
    DWORD result;
    memcpy(&result, data, sizeof(result));
    return result;
}

static bool removeSavedCount(std::vector<BYTE>& data, bool columns) {
    const size_t header = columns ? 40 : 20;
    const size_t countOffset = columns ? 32 : 16;
    const size_t recordSize = 24;
    if (data.size() < header) return false;
    if (columns && (readNumber(&data[16]) != 0xfddfdffd ||
                    readNumber(&data[20]) != 16 || readNumber(&data[36]) != recordSize)) return false;
    const DWORD count = readNumber(&data[countOffset]);
    if (count != (data.size() - header) / recordSize || (data.size() - header) % recordSize)
        return false;
    static_assert(sizeof(PROPERTYKEY) == 20);
    std::vector<BYTE> result(data.begin(), data.begin() + header);
    for (size_t pos = header; pos < data.size(); pos += recordSize) {
        if (memcmp(&data[pos], &PKEY_FileCount, sizeof(PROPERTYKEY)) != 0)
            result.insert(result.end(), data.begin() + pos, data.begin() + pos + recordSize);
    }
    if (result.size() == data.size()) return false;
    if (result.size() == header) {
        result.resize(header + recordSize);
        memcpy(&result[header], &PKEY_ItemNameDisplay, sizeof(PROPERTYKEY));
        DWORD widthOrDirection = columns ? 240 : SORT_ASCENDING;
        memcpy(&result[header + sizeof(PROPERTYKEY)], &widthOrDirection, sizeof(DWORD));
    }
    DWORD remaining = static_cast<DWORD>((result.size() - header) / recordSize);
    memcpy(&result[countOffset], &remaining, sizeof(remaining));
    data.swap(result);
    return true;
}

static void cleanSavedValue(HKEY key, const wchar_t* name, bool columns) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < 20 || bytes > 1024 * 1024) return;
    std::vector<BYTE> old(bytes);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_BINARY, nullptr, old.data(), &bytes) != ERROR_SUCCESS) return;
    old.resize(bytes);
    auto replacement = old;
    if (!removeSavedCount(replacement, columns)) return;
    // Recheck before writing in case Explorer just saved a different layout.
    std::vector<BYTE> current(bytes);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_BINARY, nullptr, current.data(), &bytes) != ERROR_SUCCESS ||
        bytes != old.size() || current != old) return;
    RegSetKeyValueW(key, nullptr, name, REG_BINARY, replacement.data(), static_cast<DWORD>(replacement.size()));
}

static void cleanSavedViews(HKEY parent, const wchar_t* subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    HKEY values = nullptr;
    if (RegOpenKeyExW(key, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &values) == ERROR_SUCCESS) {
        cleanSavedValue(values, L"ColInfo", true);
        cleanSavedValue(values, L"Sort", false);
        RegCloseKey(values);
    }
    for (DWORD index = 0; ; ++index) {
        wchar_t child[256];
        DWORD size = ARRAYSIZE(child);
        if (RegEnumKeyExW(key, index, child, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        cleanSavedViews(key, child);
    }
    RegCloseKey(key);
}

static void cleanOpenView(IShellView* shell, IFolderView2* view) {
    int count = 0;
    if (SUCCEEDED(view->GetSortColumnCount(&count)) && count > 0) {
        std::vector<SORTCOLUMN> sort(count);
        if (SUCCEEDED(view->GetSortColumns(sort.data(), count))) {
            auto end = std::remove_if(sort.begin(), sort.end(), [](const SORTCOLUMN& col) {
                return IsEqualPropertyKey(col.propkey, PKEY_FileCount);
            });
            if (end != sort.end()) {
                sort.erase(end, sort.end());
                if (sort.empty()) sort.push_back({PKEY_ItemNameDisplay, SORT_ASCENDING});
                view->SetSortColumns(sort.data(), static_cast<int>(sort.size()));
            }
        }
    }
    ComPtr<IColumnManager> manager;
    UINT visible = 0;
    if (FAILED(shell->QueryInterface(IID_PPV_ARGS(&manager))) ||
        FAILED(manager->GetColumnCount(CM_ENUM_VISIBLE, &visible)) || !visible) return;
    std::vector<PROPERTYKEY> keys(visible);
    if (FAILED(manager->GetColumns(CM_ENUM_VISIBLE, keys.data(), visible))) return;
    auto end = std::remove_if(keys.begin(), keys.end(), [](const PROPERTYKEY& key) {
        return IsEqualPropertyKey(key, PKEY_FileCount);
    });
    if (end == keys.end()) return;
    keys.erase(end, keys.end());
    if (keys.empty()) keys.push_back(PKEY_ItemNameDisplay);
    std::vector<CM_COLUMNINFO> info(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        info[i].cbSize = sizeof(CM_COLUMNINFO);
        info[i].dwMask = CM_MASK_WIDTH | CM_MASK_STATE;
        if (FAILED(manager->GetColumnInfo(keys[i], &info[i]))) info[i].dwMask = 0;
    }
    if (SUCCEEDED(manager->SetColumns(keys.data(), static_cast<UINT>(keys.size())))) {
        for (size_t i = 0; i < keys.size(); ++i)
            if (info[i].dwMask) manager->SetColumnInfo(keys[i], &info[i]);
    }
}
