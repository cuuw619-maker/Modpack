// ==WindhawkMod==
// @id              win11-dock-taskbar
// @name            Win11 Dock Taskbar
// @description     Custom Windows 11 taskbar magnification with edge-anchored icons.
// @version         0.2.0
// @author          cuuw619-maker
// @github          https://github.com/cuuw619-maker/Modpack
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lshcore -lwindowsapp -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Win11 Dock Taskbar

A custom Windows 11 taskbar hover effect.

The important visual difference from a centered scale is the transform origin:
on the normal bottom taskbar icons are anchored to their bottom edge, so an
enlarged icon grows upward out of the taskbar instead of disappearing beneath it.

The effect is calculated from the cursor distance to each application button.
Nearby buttons scale less and are displaced to keep the row readable.

Only Windows 11 Explorer taskbar elements are modified.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enable Dock effect
- maxScale: 145
  $name: Maximum icon scale (%)
  $description: 145 means the closest icon becomes 45% larger.
- radius: 120
  $name: Effect radius (px)
  $description: Horizontal/vertical distance around the cursor.
- spacing: 55
  $name: Neighbor spacing (%)
  $description: How much the growing icons push nearby icons apart.
- smoothing: 18
  $name: Smoothing
  $description: Higher values react faster. Lower values feel softer.
- edgeLift: 2
  $name: Outward lift (px)
  $description: Adds a small outward movement while an icon grows.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#undef GetCurrentTime

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Input;

namespace {

enum class DockEdge {
    Bottom,
    Top,
    Left,
    Right
};

struct Settings {
    bool enabled = true;
    double maxScale = 1.45;
    double radius = 120.0;
    double spacing = 0.55;
    double smoothing = 18.0;
    double edgeLift = 2.0;
};

struct IconState {
    weak_ref<FrameworkElement> element;
    ScaleTransform scale{nullptr};
    TranslateTransform translate{nullptr};
    double center = 0.0;
    double size = 0.0;
    double currentScale = 1.0;
    double currentShift = 0.0;
};

struct TaskbarState {
    weak_ref<FrameworkElement> frame;
    weak_ref<FrameworkElement> host;
    HWND hwnd = nullptr;
    DockEdge edge = DockEdge::Bottom;
    std::vector<IconState> icons;
    bool pointerInside = false;
    double cursorAxis = 0.0;
    uint64_t generation = 0;
};

Settings g_settings;
std::map<void*, TaskbarState> g_taskbars;

event_token g_renderToken{};
bool g_renderHooked = false;
bool g_unloading = false;

using OnPointerMoved_t = int(WINAPI*)(void*, void*);
using OnPointerExited_t = int(WINAPI*)(void*, void*);

OnPointerMoved_t OnPointerMoved_Original = nullptr;
OnPointerExited_t OnPointerExited_Original = nullptr;

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

constexpr wchar_t kClassName[] = L"Taskbar.TaskbarFrame";
bool g_taskbarHookInstalled = false;

void CALLBACK OnRendering(
    const Windows::Foundation::IInspectable&,
    const Windows::Foundation::IInspectable&);

HWND GetTaskbarWindowForThread() {
    const DWORD tid = GetCurrentThreadId();

    HWND main = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (main && GetWindowThreadProcessId(main, nullptr) == tid) {
        return main;
    }

    HWND second = nullptr;
    while ((second = FindWindowExW(
                nullptr, second, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        if (GetWindowThreadProcessId(second, nullptr) == tid) {
            return second;
        }
    }

    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        const HMONITOR monitor =
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

        if (main &&
            MonitorFromWindow(main, MONITOR_DEFAULTTONULL) == monitor) {
            return main;
        }

        second = nullptr;
        while ((second = FindWindowExW(
                    nullptr, second, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
            if (MonitorFromWindow(second, MONITOR_DEFAULTTONULL) == monitor) {
                return second;
            }
        }
    }

    return main;
}

DockEdge DetectEdge(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return DockEdge::Bottom;
    }

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) {
        return DockEdge::Bottom;
    }

    const LONG width = r.right - r.left;
    const LONG height = r.bottom - r.top;

    if (width < height) {
        HMONITOR monitor =
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(monitor, &mi)) {
            const LONG middle = mi.rcMonitor.left +
                (mi.rcMonitor.right - mi.rcMonitor.left) / 2;
            return r.left < middle ? DockEdge::Left : DockEdge::Right;
        }

        return DockEdge::Left;
    }

    HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        const LONG middle = mi.rcMonitor.top +
            (mi.rcMonitor.bottom - mi.rcMonitor.top) / 2;
        return r.top < middle ? DockEdge::Top : DockEdge::Bottom;
    }

    return DockEdge::Bottom;
}

bool IsTaskButton(FrameworkElement const& element) {
    if (!element) {
        return false;
    }

    const hstring name = get_class_name(element);

    // Windows 11 builds have used this class for the actual task list button.
    return name == L"Taskbar.TaskListButton";
}

void CollectTaskButtons(
    FrameworkElement const& root,
    std::vector<FrameworkElement>& out,
    std::set<void*>& seen) {

    if (!root) {
        return;
    }

    const int count = VisualTreeHelper::GetChildrenCount(root);

    for (int i = 0; i < count; ++i) {
        auto child =
            VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();

        if (!child) {
            continue;
        }

        if (IsTaskButton(child)) {
            void* key = get_abi(child);
            if (key && seen.insert(key).second &&
                child.Visibility() == Visibility::Visible &&
                child.ActualWidth() > 1.0 &&
                child.ActualHeight() > 1.0) {
                out.push_back(child);
            }

            // Do not descend into a task button. We only want one transform
            // per application button.
            continue;
        }

        CollectTaskButtons(child, out, seen);
    }
}

FrameworkElement FindButtonHost(
    FrameworkElement const& frame,
    FrameworkElement const& firstButton) {

    if (!frame || !firstButton) {
        return nullptr;
    }

    FrameworkElement current = firstButton;

    // Walk upward only a few levels. The common Win11 layout contains a
    // repeater/panel directly above the TaskListButton elements.
    for (int i = 0; i < 6; ++i) {
        auto parent = current.Parent().try_as<FrameworkElement>();
        if (!parent) {
            break;
        }

        const int childCount = VisualTreeHelper::GetChildrenCount(parent);
        if (childCount >= 2 && childCount <= 100) {
            return parent;
        }

        if (get_abi(parent) == get_abi(frame)) {
            break;
        }

        current = parent;
    }

    return nullptr;
}

void ConfigureTransform(TaskbarState& state, IconState& icon) {
    auto element = icon.element.get();
    if (!element) {
        return;
    }

    try {
        auto transformGroup =
            element.RenderTransform().try_as<TransformGroup>();

        if (!transformGroup) {
            transformGroup = TransformGroup();

            auto existing = element.RenderTransform();
            if (existing) {
                transformGroup.Children().Append(existing);
            }

            icon.scale = ScaleTransform();
            icon.translate = TranslateTransform();

            transformGroup.Children().Append(icon.scale);
            transformGroup.Children().Append(icon.translate);
            element.RenderTransform(transformGroup);
        } else {
            // Use a private pair appended by this mod. We don't destroy a
            // transform that belongs to the shell.
            const auto children = transformGroup.Children();

            if (children.Size() >= 2) {
                icon.scale =
                    children.GetAt(children.Size() - 2).try_as<ScaleTransform>();
                icon.translate =
                    children.GetAt(children.Size() - 1).try_as<TranslateTransform>();
            }

            if (!icon.scale || !icon.translate) {
                icon.scale = ScaleTransform();
                icon.translate = TranslateTransform();
                transformGroup.Children().Append(icon.scale);
                transformGroup.Children().Append(icon.translate);
            }
        }

        element.RenderTransformOrigin({0.5f, 0.5f});
        element.SetValue(Controls::Panel::ZIndexProperty(), winrt::box_value(1000));

        switch (state.edge) {
            case DockEdge::Bottom:
                element.RenderTransformOrigin({0.5f, 1.0f});
                break;
            case DockEdge::Top:
                element.RenderTransformOrigin({0.5f, 0.0f});
                break;
            case DockEdge::Left:
                element.RenderTransformOrigin({0.0f, 0.5f});
                break;
            case DockEdge::Right:
                element.RenderTransformOrigin({1.0f, 0.5f});
                break;
        }

        element.Clip(nullptr);

        if (icon.scale) {
            icon.scale.ScaleX(icon.currentScale);
            icon.scale.ScaleY(icon.currentScale);
        }
        if (icon.translate) {
            if (state.edge == DockEdge::Bottom ||
                state.edge == DockEdge::Top) {
                icon.translate.X(icon.currentShift);
            } else {
                icon.translate.Y(icon.currentShift);
            }
        }
    } catch (...) {
    }
}

void RefreshIcons(TaskbarState& state) {
    auto frame = state.frame.get();
    if (!frame) {
        return;
    }

    std::vector<FrameworkElement> elements;
    std::set<void*> seen;
    CollectTaskButtons(frame, elements, seen);

    std::vector<IconState> next;
    next.reserve(elements.size());

    for (const auto& element : elements) {
        try {
            auto transform = element.TransformToVisual(frame);
            const auto point = transform.TransformPoint({0, 0});

            IconState icon;
            icon.element = element;
            icon.size =
                (state.edge == DockEdge::Left || state.edge == DockEdge::Right)
                    ? element.ActualHeight()
                    : element.ActualWidth();

            const double position =
                (state.edge == DockEdge::Left || state.edge == DockEdge::Right)
                    ? point.Y
                    : point.X;

            icon.center = position + icon.size * 0.5;
            icon.currentScale = 1.0;
            icon.currentShift = 0.0;

            // Preserve a running animation for unchanged visual elements.
            for (const auto& old : state.icons) {
                auto oldElement = old.element.get();
                if (oldElement &&
                    get_abi(oldElement) == get_abi(element)) {
                    icon.currentScale = old.currentScale;
                    icon.currentShift = old.currentShift;
                    break;
                }
            }

            ConfigureTransform(state, icon);
            next.push_back(std::move(icon));
        } catch (...) {
        }
    }

    std::sort(
        next.begin(), next.end(),
        [](const IconState& a, const IconState& b) {
            return a.center < b.center;
        });

    state.icons = std::move(next);

    if (!state.host && !state.icons.empty()) {
        state.host = FindButtonHost(frame, state.icons.front().element.get());
    }
}

double Falloff(double distance) {
    if (distance >= g_settings.radius) {
        return 0.0;
    }

    const double t =
        std::clamp(distance / std::max(1.0, g_settings.radius), 0.0, 1.0);

    // Cosine gives a soft center and a clean zero at the radius edge.
    return (std::cos(t * 3.141592653589793) + 1.0) * 0.5;
}

void ApplyDock(TaskbarState& state, double dt) {
    const bool vertical =
        state.edge == DockEdge::Left || state.edge == DockEdge::Right;

    const double alpha = std::clamp(
        1.0 - std::exp(-std::max(0.1, g_settings.smoothing) * dt),
        0.0, 1.0);

    std::vector<double> targets(state.icons.size());
    std::vector<double> expansion(state.icons.size());

    double totalExpansion = 0.0;

    for (size_t i = 0; i < state.icons.size(); ++i) {
        auto& icon = state.icons[i];
        const double distance =
            std::abs(icon.center - state.cursorAxis);

        const double influence = Falloff(distance);
        targets[i] =
            1.0 + (g_settings.maxScale - 1.0) * influence;

        expansion[i] =
            icon.size * (targets[i] - 1.0) * g_settings.spacing;

        totalExpansion += expansion[i];
    }

    double accumulated = 0.0;

    for (size_t i = 0; i < state.icons.size(); ++i) {
        auto& icon = state.icons[i];
        auto element = icon.element.get();

        if (!element || !icon.scale || !icon.translate) {
            continue;
        }

        const double centeredShift =
            accumulated + expansion[i] * 0.5 - totalExpansion * 0.5;

        accumulated += expansion[i];

        const double targetShift =
            centeredShift * (state.pointerInside ? 1.0 : 0.0);

        const double targetScale =
            state.pointerInside ? targets[i] : 1.0;

        icon.currentScale +=
            (targetScale - icon.currentScale) * alpha;
        icon.currentShift +=
            (targetShift - icon.currentShift) * alpha;

        icon.scale.ScaleX(icon.currentScale);
        icon.scale.ScaleY(icon.currentScale);

        if (vertical) {
            icon.translate.X(
                state.edge == DockEdge::Left
                    ? g_settings.edgeLift * (icon.currentScale - 1.0)
                    : -g_settings.edgeLift * (icon.currentScale - 1.0));

            icon.translate.Y(icon.currentShift);
        } else {
            icon.translate.X(icon.currentShift);
            icon.translate.Y(
                state.edge == DockEdge::Bottom
                    ? -g_settings.edgeLift * (icon.currentScale - 1.0)
                    : g_settings.edgeLift * (icon.currentScale - 1.0));
        }

        if (!state.pointerInside &&
            std::abs(icon.currentScale - 1.0) < 0.001 &&
            std::abs(icon.currentShift) < 0.05) {
            icon.currentScale = 1.0;
            icon.currentShift = 0.0;
            icon.scale.ScaleX(1.0);
            icon.scale.ScaleY(1.0);
        }
    }
}

void StartRendering() {
    if (g_renderHooked) {
        return;
    }

    g_renderHooked = true;
    g_renderToken =
        CompositionTarget::Rendering(&OnRendering);
}

void StopRendering() {
    if (!g_renderHooked) {
        return;
    }

    CompositionTarget::Rendering(g_renderToken);
    g_renderHooked = false;
}

void CALLBACK OnRendering(
    const Windows::Foundation::IInspectable&,
    const Windows::Foundation::IInspectable&) {

    if (g_unloading) {
        return;
    }

    static ULONGLONG lastTick = GetTickCount64();
    const ULONGLONG now = GetTickCount64();

    const double dt =
        std::clamp((now - lastTick) / 1000.0, 0.001, 0.05);
    lastTick = now;

    bool anythingActive = false;

    for (auto& pair : g_taskbars) {
        auto& state = pair.second;

        if (!state.pointerInside) {
            bool settling = false;

            for (auto& icon : state.icons) {
                if (std::abs(icon.currentScale - 1.0) > 0.01 ||
                    std::abs(icon.currentShift) > 0.2) {
                    settling = true;
                    break;
                }
            }

            if (!settling) {
                continue;
            }
        }

        if (state.icons.empty() || ++state.generation % 30 == 0) {
            RefreshIcons(state);
        }

        ApplyDock(state, dt);
        anythingActive = true;
    }

    if (!anythingActive) {
        StopRendering();
    }
}

void EnsureState(void* key, FrameworkElement const& frame) {
    auto it = g_taskbars.find(key);

    if (it == g_taskbars.end()) {
        TaskbarState state;
        state.frame = frame;
        state.hwnd = GetTaskbarWindowForThread();
        state.edge = DetectEdge(state.hwnd);
        g_taskbars.emplace(key, std::move(state));
        it = g_taskbars.find(key);
    } else {
        it->second.frame = frame;

        HWND hwnd = GetTaskbarWindowForThread();
        if (hwnd != it->second.hwnd) {
            it->second.hwnd = hwnd;
            it->second.edge = DetectEdge(hwnd);
        }
    }

    auto& state = it->second;
    if (state.icons.empty()) {
        RefreshIcons(state);
    }
}

int WINAPI OnPointerMoved_Hook(void* thisPtr, void* argsPtr) {
    auto original = OnPointerMoved_Original(thisPtr, argsPtr);

    if (g_unloading || !thisPtr || !argsPtr) {
        return original;
    }

    try {
        FrameworkElement frame = nullptr;
        reinterpret_cast<IUnknown*>(thisPtr)->QueryInterface(
            guid_of<FrameworkElement>(), put_abi(frame));

        if (!frame || get_class_name(frame) != kClassName) {
            return original;
        }

        PointerRoutedEventArgs args = nullptr;
        reinterpret_cast<IUnknown*>(argsPtr)->QueryInterface(
            guid_of<PointerRoutedEventArgs>(), put_abi(args));

        if (!args) {
            return original;
        }

        EnsureState(thisPtr, frame);

        auto& state = g_taskbars.at(thisPtr);
        state.pointerInside = g_settings.enabled;
        state.edge = DetectEdge(state.hwnd);

        auto pointerPoint = args.GetCurrentPoint(frame);
        auto point = pointerPoint.Position();
        state.cursorAxis =
            (state.edge == DockEdge::Left ||
             state.edge == DockEdge::Right)
                ? point.Y
                : point.X;

        StartRendering();
    } catch (...) {
    }

    return original;
}

int WINAPI OnPointerExited_Hook(void* thisPtr, void* argsPtr) {
    auto original = OnPointerExited_Original(thisPtr, argsPtr);

    if (g_unloading || !thisPtr) {
        return original;
    }

    auto it = g_taskbars.find(thisPtr);
    if (it != g_taskbars.end()) {
        it->second.pointerInside = false;
        StartRendering();
    }

    return original;
}

bool HookTaskbarView(HMODULE module) {
    if (g_taskbarHookInstalled) {
        return true;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {
                LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerMoved(void *))"
            },
            &OnPointerMoved_Original,
            OnPointerMoved_Hook
        },
        {
            {
                LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerExited(void *))"
            },
            &OnPointerExited_Original,
            OnPointerExited_Hook
        }
    };

    if (!HookSymbols(module, hooks, ARRAYSIZE(hooks))) {
        return false;
    }

    g_taskbarHookInstalled = true;
    return true;
}

HMODULE FindTaskbarViewModule() {
    HMODULE module = GetModuleHandleW(L"Taskbar.View.dll");
    if (!module) {
        module = GetModuleHandleW(L"ExplorerExtensions.dll");
    }
    return module;
}

HMODULE WINAPI LoadLibraryExW_Hook(
    LPCWSTR name, HANDLE file, DWORD flags) {

    HMODULE module =
        LoadLibraryExW_Original(name, file, flags);

    if (!module || g_unloading) {
        return module;
    }

    if (module == FindTaskbarViewModule()) {
        if (HookTaskbarView(module)) {
            Wh_ApplyHookOperations();
        }
    }

    return module;
}

void LoadSettings() {
    g_settings.enabled = Wh_GetIntSetting(L"enabled") != 0;

    int scale = Wh_GetIntSetting(L"maxScale");
    scale = std::clamp(scale, 105, 200);
    g_settings.maxScale = scale / 100.0;

    int radius = Wh_GetIntSetting(L"radius");
    radius = std::clamp(radius, 30, 250);
    g_settings.radius = static_cast<double>(radius);

    int spacing = Wh_GetIntSetting(L"spacing");
    spacing = std::clamp(spacing, 0, 100);
    g_settings.spacing = spacing / 100.0;

    int smoothing = Wh_GetIntSetting(L"smoothing");
    smoothing = std::clamp(smoothing, 1, 60);
    g_settings.smoothing = static_cast<double>(smoothing);

    int lift = Wh_GetIntSetting(L"edgeLift");
    lift = std::clamp(lift, 0, 20);
    g_settings.edgeLift = static_cast<double>(lift);
}

} // namespace

BOOL Wh_ModInit() {
    LoadSettings();

    HMODULE module = FindTaskbarViewModule();

    if (module) {
        if (!HookTaskbarView(module)) {
            Wh_Log(L"Win11DockTaskbar: Taskbar.View hook failed");
            return FALSE;
        }
    } else {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto loadLibrary =
            reinterpret_cast<LoadLibraryExW_t>(
                GetProcAddress(kernelBase, "LoadLibraryExW"));

        if (!loadLibrary) {
            return FALSE;
        }

        if (!Wh_SetFunctionHook(
                reinterpret_cast<void*>(loadLibrary),
                reinterpret_cast<void*>(LoadLibraryExW_Hook),
                reinterpret_cast<void**>(&LoadLibraryExW_Original))) {
            return FALSE;
        }
    }

    if (module) {
        Wh_ApplyHookOperations();
    }

    Wh_Log(L"Win11 Dock Taskbar initialized");
    return TRUE;
}

void Wh_ModUninit() {
    g_unloading = true;

    if (g_renderHooked) {
        StopRendering();
    }

    for (auto& pair : g_taskbars) {
        for (auto& icon : pair.second.icons) {
            try {
                auto element = icon.element.get();
                if (!element) {
                    continue;
                }

                if (icon.scale) {
                    icon.scale.ScaleX(1.0);
                    icon.scale.ScaleY(1.0);
                }

                if (icon.translate) {
                    icon.translate.X(0.0);
                    icon.translate.Y(0.0);
                }

                element.SetValue(Controls::Panel::ZIndexProperty(), winrt::box_value(0));
                element.RenderTransformOrigin({0.5f, 0.5f});
            } catch (...) {
            }
        }
    }

    g_taskbars.clear();

    Wh_Log(L"Win11 Dock Taskbar uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
