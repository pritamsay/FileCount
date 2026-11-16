#pragma once
#include <UIAutomation.h>
#include <map>
#include <algorithm>

struct IconLabel {
    int x, y;
    wstring text;
    bool operator==(const IconLabel& b) const { return x == b.x && y == b.y && text == b.text; }
};

class IconBadges {
    HWND window = nullptr;
    RECT previous = {};
    std::vector<IconLabel> labels;
    int dpi = 96;
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationCondition> listCondition, itemCondition;
    ComPtr<IUIAutomationCacheRequest> cache;
    static constexpr COLORREF clearColor = RGB(255, 0, 255);

    static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_NCCREATE) {
            auto create = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            auto self = reinterpret_cast<IconBadges*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) self->draw(dc, paint.rcPaint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }

    int scale(int value) const { return MulDiv(value, dpi, 96); }

    void draw(HDC dc, const RECT& region) {
        HBRUSH background = CreateSolidBrush(clearColor);
        FillRect(dc, &region, background);
        DeleteObject(background);
        HFONT font = CreateFontW(-scale(13), 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        auto oldFont = SelectObject(dc, font);
        HBRUSH blue = CreateSolidBrush(RGB(27, 68, 110));
        auto oldBrush = SelectObject(dc, blue);
        auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        for (const auto& label : labels) {
            int length = static_cast<int>(label.text.size());
            SIZE size;
            GetTextExtentPoint32W(dc, label.text.c_str(), length, &size);
            int diameter = (std::max)(scale(18), (std::max)(int(size.cx), int(size.cy)) + scale(5));
            int x = label.x - diameter;
            Ellipse(dc, x, label.y, label.x, label.y + diameter);
            TextOutW(dc, x + (diameter - size.cx) / 2, label.y + (diameter - size.cy) / 2,
                     label.text.c_str(), length);
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldFont);
        DeleteObject(blue);
        DeleteObject(font);
    }

    bool prepare(HWND owner) {
        if (!window) {
            WNDCLASSW type = {};
            type.lpfnWndProc = procedure;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"FolderFileCountBadges";
            RegisterClassW(&type);
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                type.lpszClassName, L"Folder counts", WS_POPUP, 0, 0, 1, 1, owner, nullptr, type.hInstance, this);
            if (!window) return false;
            SetLayeredWindowAttributes(window, clearColor, 255, LWA_COLORKEY);
        }
        if (GetWindow(window, GW_OWNER) != owner)
            SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        if (cache) return true;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return false;
        VARIANT value = {};
        value.vt = VT_I4;
        value.lVal = UIA_ListControlTypeId;
        if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &listCondition))) return false;
        value.lVal = UIA_ListItemControlTypeId;
        if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &itemCondition))) return false;
        if (FAILED(automation->CreateCacheRequest(&cache))) return false;
        cache->AddProperty(UIA_NamePropertyId);
        cache->AddProperty(UIA_BoundingRectanglePropertyId);
        cache->AddProperty(UIA_IsOffscreenPropertyId);
        return true;
    }

public:
    ~IconBadges() { if (window && IsWindow(window)) DestroyWindow(window); }

    void hide() { if (window && IsWindowVisible(window)) ShowWindow(window, SW_HIDE); }

    void display(HWND owner, IShellView* shell, int iconSize, const std::map<wstring, ULONGLONG>& names) {
        HWND pane = nullptr;
        if (names.empty() || FAILED(shell->GetWindow(&pane)) || !prepare(owner)) { hide(); return; }
        ComPtr<IUIAutomationElement> root, list;
        if (FAILED(automation->ElementFromHandle(pane, &root)) ||
            FAILED(root->FindFirst(TreeScope_Subtree, listCondition.Get(), &list)) || !list) { hide(); return; }
        RECT viewport = {};
        if (FAILED(list->get_CurrentBoundingRectangle(&viewport)) || IsRectEmpty(&viewport)) { hide(); return; }
        ComPtr<IUIAutomationElementArray> rows;
        if (FAILED(list->FindAllBuildCache(TreeScope_Children, itemCondition.Get(), cache.Get(), &rows))) { hide(); return; }
        std::vector<IconLabel> visible;
        int total = 0;
        rows->get_Length(&total);
        dpi = GetDpiForWindow(owner);
        for (int i = 0; i < total; ++i) {
            ComPtr<IUIAutomationElement> row;
            if (FAILED(rows->GetElement(i, &row))) continue;
            BOOL offscreen = TRUE;
            row->get_CachedIsOffscreen(&offscreen);
            if (offscreen) continue;
            BSTR title = nullptr;
            row->get_CachedName(&title);
            auto found = names.find(title ? title : L"");
            SysFreeString(title);
            if (found == names.end() || found->second == UnknownCount) continue;
            RECT r = {};
            if (FAILED(row->get_CachedBoundingRectangle(&r)) || IsRectEmpty(&r)) continue;
            int right = (r.left + r.right) / 2 + (std::min)(scale(iconSize / 2), int(r.right-r.left)/2 - 2);
            if (iconSize <= 24) right = r.left + scale(iconSize + 3);
            int y = r.top + scale(2);
            if (y < viewport.top || y + scale(17) > viewport.bottom || right > viewport.right || right <= viewport.left) continue;
            visible.push_back({right - viewport.left, y - viewport.top, std::to_wstring(found->second)});
        }
        if (visible.empty() || IsIconic(owner) || !IsWindowVisible(owner)) { hide(); return; }
        // Place the badges immediately above this Explorer window. Apps that
        // overlap an inactive Explorer window must also cover its badges.
        HWND above = GetWindow(owner, GW_HWNDPREV);
        bool inPlace = above == window;
        if (inPlace && IsWindowVisible(window) && EqualRect(&viewport, &previous) && labels == visible) return;
        labels = std::move(visible);
        previous = viewport;
        SetWindowPos(window, above, viewport.left, viewport.top, viewport.right - viewport.left,
                     viewport.bottom - viewport.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | (inPlace ? SWP_NOZORDER : 0));
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
};
