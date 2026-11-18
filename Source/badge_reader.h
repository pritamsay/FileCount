#pragma once
#include <UIAutomation.h>

struct IconLabel {
    int x, y;
    wstring text;
    bool operator==(const IconLabel& b) const { return x == b.x && y == b.y && text == b.text; }
};

struct BadgeLayout {
    HWND pane = nullptr;
    int iconSize = 0, dpi = 96;
    RECT viewport = {};
    std::map<wstring, ULONGLONG> names;
    std::vector<IconLabel> labels;
    std::atomic<bool> done{false}, cancelled{false};
};

// UI Automation must not run on a thread that owns windows. It can synchronously
// send messages back to the provider. This single MTA reader owns no windows.
class BadgeReader {
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::shared_ptr<BadgeLayout> pending;
        bool stopping = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    static void read(IUIAutomation* automation, BadgeLayout& job) {
        ComPtr<IUIAutomationCondition> listType, itemType, onScreen, visibleItems;
        VARIANT value = {};
        value.vt = VT_I4; value.lVal = UIA_ListControlTypeId;
        automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &listType);
        value.lVal = UIA_ListItemControlTypeId;
        automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &itemType);
        value.vt = VT_BOOL; value.boolVal = VARIANT_FALSE;
        automation->CreatePropertyCondition(UIA_IsOffscreenPropertyId, value, &onScreen);
        if (!listType || !itemType || !onScreen ||
            FAILED(automation->CreateAndCondition(itemType.Get(), onScreen.Get(), &visibleItems))) return;
        ComPtr<IUIAutomationCacheRequest> cache;
        if (FAILED(automation->CreateCacheRequest(&cache))) return;
        cache->AddProperty(UIA_NamePropertyId);
        cache->AddProperty(UIA_BoundingRectanglePropertyId);
        cache->AddProperty(UIA_IsOffscreenPropertyId);
        cache->put_AutomationElementMode(AutomationElementMode_None);
        ComPtr<IUIAutomationElement> root, list;
        if (job.cancelled || FAILED(automation->ElementFromHandle(job.pane, &root)) ||
            FAILED(root->FindFirst(TreeScope_Subtree, listType.Get(), &list)) || !list ||
            FAILED(list->get_CurrentBoundingRectangle(&job.viewport)) || IsRectEmpty(&job.viewport)) return;
        ComPtr<IUIAutomationElementArray> rows;
        if (job.cancelled || FAILED(list->FindAllBuildCache(TreeScope_Children, visibleItems.Get(), cache.Get(), &rows))) return;
        int total = 0;
        rows->get_Length(&total);
        auto scale = [&](int n) { return MulDiv(n, job.dpi, 96); };
        for (int i = 0; i < total && !job.cancelled; ++i) {
            ComPtr<IUIAutomationElement> row;
            if (FAILED(rows->GetElement(i, &row))) continue;
            BSTR name = nullptr;
            row->get_CachedName(&name);
            auto found = job.names.find(name ? name : L"");
            SysFreeString(name);
            if (found == job.names.end() || found->second == UnknownCount) continue;
            RECT r = {};
            if (FAILED(row->get_CachedBoundingRectangle(&r)) || IsRectEmpty(&r)) continue;
            int right = (r.left + r.right) / 2 + (std::min)(scale(job.iconSize / 2), int(r.right-r.left)/2 - 2);
            if (job.iconSize <= 24) right = r.left + scale(job.iconSize + 3);
            int y = r.top + scale(2);
            if (y < job.viewport.top || y + scale(17) > job.viewport.bottom ||
                right > job.viewport.right || right <= job.viewport.left) continue;
            job.labels.push_back({right - job.viewport.left, y - job.viewport.top, std::to_wstring(found->second)});
        }
    }
    static void run(std::shared_ptr<State> state) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        {
            ComPtr<IUIAutomation2> timed;
            ComPtr<IUIAutomation> automation;
            if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&timed)))) {
                timed->put_ConnectionTimeout(200);
                timed->put_TransactionTimeout(200);
                timed.As(&automation);
            }
            for (;;) {
                std::shared_ptr<BadgeLayout> job;
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->stopping || state->pending; });
                    if (state->stopping) break;
                    job = std::move(state->pending);
                }
                if (automation && !job->cancelled) read(automation.Get(), *job);
                job->done.store(true, std::memory_order_release);
            }
        }
        CoUninitialize();
    }
public:
    BadgeReader() { std::thread(run, state).detach(); }
    ~BadgeReader() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
        state->wake.notify_one();
    }
    bool submit(const std::shared_ptr<BadgeLayout>& job) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->pending) return false;
        state->pending = job;
        state->wake.notify_one();
        return true;
    }
};
