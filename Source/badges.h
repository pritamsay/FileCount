#pragma once
#include "badge_reader.h"
#include <map>
#include <algorithm>

class IconBadges {
    HWND window = nullptr;
    RECT previous = {};
    std::vector<IconLabel> labels;
    int dpi = 96;
    bool dirty = true;
    ULONGLONG lastLayout = 0;
    std::shared_ptr<BadgeLayout> pending;
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
        return true;
    }

public:
    ~IconBadges() { if (pending) pending->cancelled = true; if (window && IsWindow(window)) DestroyWindow(window); }

    void hide() {
        if (pending) pending->cancelled = true;
        dirty = true;
        if (window && IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
    }
    void invalidate() { hide(); }

    void display(HWND owner, IShellView* shell, int iconSize,
                 const std::map<wstring, ULONGLONG>& names, BadgeReader& reader) {
        HWND pane = nullptr;
        if (names.empty() || FAILED(shell->GetWindow(&pane)) || !prepare(owner)) { hide(); return; }
        auto now = GetTickCount64();
        if (pending && pending->done.load(std::memory_order_acquire)) {
            if (!pending->cancelled) {
                labels = std::move(pending->labels);
                previous = pending->viewport;
                dpi = pending->dpi;
                if (labels.empty()) { if (window) ShowWindow(window, SW_HIDE); }
                else {
                    HWND above = GetWindow(owner, GW_HWNDPREV);
                    SetWindowPos(window, above, previous.left, previous.top,
                        previous.right - previous.left, previous.bottom - previous.top,
                        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | (above == window ? SWP_NOZORDER : 0));
                    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE);
                }
            }
            pending.reset();
        }
        // One pending read per view, coalesced after layout/scroll events.
        if (window && IsWindowVisible(window)) {
            HWND above = GetWindow(owner, GW_HWNDPREV);
            if (above != window) SetWindowPos(window, above, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (pending || now - lastLayout < 500 || (!dirty && now - lastLayout < 3000)) return;
        auto job = std::make_shared<BadgeLayout>();
        job->pane = pane; job->iconSize = iconSize; job->dpi = GetDpiForWindow(owner);
        job->names = names;
        if (reader.submit(job)) { pending = job; dirty = false; lastLayout = now; }
    }
};
