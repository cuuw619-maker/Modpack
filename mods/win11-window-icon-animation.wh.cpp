// ==WindhawkMod==
// @id              win11-window-icon-animation
// @name            Windows 11 Window Icon Animation
// @description     Smoothly grows windows from their Taskbar icon on launch and shrinks them back on close.
// @version         0.1.0
// @author          cuuw619-maker
// @github          https://github.com/cuuw619-maker/Modpack
// @include         *
// @architecture    x86-64
// @compilerOptions -ldwmapi -luiautomationcore -lole32 -luser32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 11 Window Icon Animation

This mod implements the "icon grows into window" effect for normal Win32
top-level windows.

Launch:
    Taskbar icon -> small window -> full window

Close:
    Full window -> small window -> Taskbar icon

The mod deliberately avoids a Genie mesh warp. It uses a lightweight
layered ghost window with scale, position and opacity interpolation.

Windows 11 Taskbar coordinates are discovered through UI Automation, so
the animation targets the real application button instead of a fixed
screen coordinate.

Borderless/UWP/Chromium-style windows can have limited support because
some frameworks don't expose a reliably capturable Win32 surface.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- launch_animation: true
  $name: Animate application launch
  $description: Grow the first visible top-level window out of its Taskbar icon.
- close_animation: true
  $name: Animate application close
  $description: Shrink the window into its Taskbar icon before closing it.
- launch_duration: 260
  $name: Launch duration (ms)
  $description: Duration of the opening animation.
- close_duration: 220
  $name: Close duration (ms)
  $description: Close duration.
- start_opacity: 0
  $name: Launch start opacity (%)
  $description: Opacity of the first launch frame.
- close_end_opacity: 5
  $name: Close end opacity (%)
  $description: Opacity at the end of the close animation.
- icon_size: 32
  $name: Animation icon size (px)
  $description: Fallback size used when the Taskbar UI Automation rectangle is unavailable.
- excluded_processes: [""]
  $name: Excluded processes
  $description: Process names that must never animate.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 5
#endif

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif

namespace {

struct Settings {
    bool launchAnimation{};
    bool closeAnimation{};
    int launchDuration{};
    int closeDuration{};
    int startOpacity{};
    int closeEndOpacity{};
    int iconSize{};
    std::vector<std::wstring> excluded;
};

Settings g_settings;
std::mutex g_stateMutex;
std::unordered_set<HWND> g_seenLaunchWindows;
std::unordered_set<HWND> g_activeWindows;
std::atomic<bool> g_unloading{false};
ATOM g_ghostClass = 0;

using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
using ShowWindowAsync_t = BOOL(WINAPI*)(HWND, int);
using DefWindowProcW_t = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);

ShowWindow_t ShowWindow_Original = nullptr;
ShowWindowAsync_t ShowWindowAsync_Original = nullptr;
DefWindowProcW_t DefWindowProcW_Original = nullptr;

constexpr wchar_t kBypassProp[] = L"Win11DockAnim.Bypass";

struct Bitmap {
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;

    ~Bitmap() {
        if (bitmap) DeleteObject(bitmap);
    }

    Bitmap() = default;
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
};

struct TaskbarTarget {
    RECT rect{};
    bool valid = false;
};

struct AnimationJob {
    HWND target = nullptr;
    Bitmap* source = nullptr;
    RECT from{};
    RECT to{};
    int duration = 260;
    BYTE startAlpha = 255;
    BYTE endAlpha = 255;
    bool closeRealWindow = false;
};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

bool IsExcluded(HWND hwnd) {
    if (!hwnd || g_settings.excluded.empty()) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    wchar_t path[MAX_PATH]{};
    DWORD len = ARRAYSIZE(path);
    bool ok = QueryFullProcessImageNameW(process, 0, path, &len) != FALSE;
    CloseHandle(process);
    if (!ok) return false;

    std::wstring exe = ToLower(path);
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? exe : exe.substr(slash + 1);

    for (const auto& item : g_settings.excluded) {
        if (item == base || item == exe) return true;
    }
    return false;
}

bool IsAnimatableWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (style & WS_CHILD) return false;
    if (!(style & WS_CAPTION)) return false;
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;

    return rect.right - rect.left >= 64 && rect.bottom - rect.top >= 48;
}

RECT GetVisualWindowRect(HWND hwnd) {
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(
            hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        GetWindowRect(hwnd, &rect);
    }
    return rect;
}

void SetCloaked(HWND hwnd, bool cloaked) {
    BOOL value = cloaked ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
}

void SetDwmTransitions(HWND hwnd, bool enabled) {
    BOOL disabled = enabled ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                           &disabled, sizeof(disabled));
}

void FreeAnimationState(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_activeWindows.erase(hwnd);
}

bool MarkAnimationActive(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_activeWindows.insert(hwnd).second;
}

bool WasLaunchSeen(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return !g_seenLaunchWindows.insert(hwnd).second;
}

Bitmap* CaptureWindow(HWND hwnd) {
    RECT rect = GetVisualWindowRect(hwnd);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (width < 2 || height < 2) return nullptr;

    auto* result = new Bitmap();
    result->width = width;
    result->height = height;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    if (!screen || !dc) {
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        delete result;
        return nullptr;
    }

    result->bitmap = CreateDIBSection(
        screen, &bi, DIB_RGB_COLORS, &result->bits, nullptr, 0);

    if (!result->bitmap) {
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        delete result;
        return nullptr;
    }

    HGDIOBJ old = SelectObject(dc, result->bitmap);

    BOOL painted = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
    if (!painted) {
        painted = PrintWindow(hwnd, dc, 0);
    }

    if (!painted) {
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        painted = BitBlt(
            dc, 0, 0, width, height, screen,
            windowRect.left, windowRect.top, SRCCOPY | CAPTUREBLT);
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    if (!painted) {
        delete result;
        return nullptr;
    }

    return result;
}

HWND FindTaskbarForMonitor(HMONITOR monitor) {
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!monitor) return primary;

    if (primary && MonitorFromWindow(primary, MONITOR_DEFAULTTONULL) == monitor) {
        return primary;
    }

    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(
                nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONULL) == monitor) {
            return secondary;
        }
    }

    return primary;
}

TaskbarTarget FindTaskbarButton(HWND appHwnd) {
    TaskbarTarget result{};

    HMONITOR monitor = MonitorFromWindow(appHwnd, MONITOR_DEFAULTTONEAREST);
    HWND tray = FindTaskbarForMonitor(monitor);
    if (!tray) return result;

    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool initialized = initResult == S_OK || initResult == S_FALSE;

    IUIAutomation* automation = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(&automation));

    if (FAILED(hr)) {
        hr = CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&automation));
    }

    if (FAILED(hr) || !automation) {
        if (initialized) CoUninitialize();
        return result;
    }

    IUIAutomationElement* trayElement = nullptr;
    if (SUCCEEDED(automation->ElementFromHandle(tray, &trayElement)) &&
        trayElement) {

        VARIANT btn{};
        btn.vt = VT_I4;
        btn.lVal = UIA_ButtonControlTypeId;

        VARIANT item{};
        item.vt = VT_I4;
        item.lVal = UIA_ListItemControlTypeId;

        IUIAutomationCondition* buttonCond = nullptr;
        IUIAutomationCondition* itemCond = nullptr;
        IUIAutomationCondition* orCond = nullptr;

        automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId, btn, &buttonCond);
        automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId, item, &itemCond);

        if (buttonCond && itemCond) {
            automation->CreateOrCondition(buttonCond, itemCond, &orCond);
        }

        IUIAutomationElementArray* elements = nullptr;
        if (orCond &&
            SUCCEEDED(trayElement->FindAll(
                TreeScope_Descendants, orCond, &elements)) &&
            elements) {

            wchar_t title[512]{};
            GetWindowTextW(appHwnd, title, ARRAYSIZE(title));
            std::wstring titleLower = ToLower(title);

            DWORD pid = 0;
            GetWindowThreadProcessId(appHwnd, &pid);

            std::wstring processName;
            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

            if (process) {
                wchar_t path[MAX_PATH]{};
                DWORD len = ARRAYSIZE(path);
                if (QueryFullProcessImageNameW(process, 0, path, &len)) {
                    std::wstring full = ToLower(path);
                    const size_t slash = full.find_last_of(L"\\/");
                    processName = slash == std::wstring::npos
                                      ? full
                                      : full.substr(slash + 1);
                    const size_t dot = processName.rfind(L'.');
                    if (dot != std::wstring::npos) processName.resize(dot);
                }
                CloseHandle(process);
            }

            int bestScore = 0;
            int length = 0;
            elements->get_Length(&length);

            for (int i = 0; i < length; ++i) {
                IUIAutomationElement* element = nullptr;
                if (FAILED(elements->GetElement(i, &element)) || !element) {
                    continue;
                }

                BSTR bstrName = nullptr;
                if (SUCCEEDED(element->get_CurrentName(&bstrName)) && bstrName) {
                    std::wstring name = ToLower(bstrName);
                    SysFreeString(bstrName);

                    if (!name.empty()) {
                        int score = 0;

                        if (!titleLower.empty() && name == titleLower) score += 1000;
                        if (!titleLower.empty() && name.find(titleLower) != std::wstring::npos) {
                            score += 500;
                        }
                        if (!processName.empty() && name.find(processName) != std::wstring::npos) {
                            score += 700;
                        }

                        if (name.find(L"start") != std::wstring::npos) score -= 500;
                        if (name.find(L"search") != std::wstring::npos) score -= 500;
                        if (name.find(L"task view") != std::wstring::npos) score -= 500;
                        if (name.find(L"widgets") != std::wstring::npos) score -= 500;

                        if (score > bestScore) {
                            RECT bounds{};
                            if (SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds)) &&
                                bounds.right > bounds.left &&
                                bounds.bottom > bounds.top) {
                                bestScore = score;
                                result.rect = bounds;
                                result.valid = true;
                            }
                        }
                    }
                }

                element->Release();
            }

            elements->Release();
        }

        if (buttonCond) buttonCond->Release();
        if (itemCond) itemCond->Release();
        if (orCond) orCond->Release();
        trayElement->Release();
    }

    automation->Release();
    if (initialized) CoUninitialize();

    return result;
}

TaskbarTarget FindTaskbarButtonRetry(HWND hwnd, int attempts, int delayMs) {
    TaskbarTarget target{};
    for (int i = 0; i < attempts && !g_unloading; ++i) {
        target = FindTaskbarButton(hwnd);
        if (target.valid) return target;
        Sleep(delayMs);
    }
    return target;
}

double EaseOutCubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

RECT InterpolateRect(const RECT& a, const RECT& b, double t) {
    RECT out{};
    out.left = static_cast<LONG>(std::lround(a.left + (b.left - a.left) * t));
    out.top = static_cast<LONG>(std::lround(a.top + (b.top - a.top) * t));
    out.right = static_cast<LONG>(std::lround(a.right + (b.right - a.right) * t));
    out.bottom = static_cast<LONG>(std::lround(a.bottom + (b.bottom - a.bottom) * t));
    return out;
}

RECT MakeStartRect(const RECT& targetRect, const RECT& iconRect) {
    const int targetW = targetRect.right - targetRect.left;
    const int targetH = targetRect.bottom - targetRect.top;

    const double aspect = targetH > 0 ? static_cast<double>(targetW) / targetH : 1.0;
    int w = std::max(20, iconRect.right - iconRect.left);
    int h = std::max(20, static_cast<int>(std::lround(w / aspect)));

    const int cx = (iconRect.left + iconRect.right) / 2;
    const int cy = (iconRect.top + iconRect.bottom) / 2;

    RECT start{
        cx - w / 2,
        cy - h / 2,
        cx + (w + 1) / 2,
        cy + (h + 1) / 2
    };
    return start;
}

class GhostWindow {
public:
    HWND hwnd = nullptr;

    bool Create() {
        hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            MAKEINTATOM(g_ghostClass),
            L"Win11DockGhost",
            WS_POPUP,
            0, 0, 1, 1,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        return hwnd != nullptr;
    }

    bool Render(const Bitmap& bitmap, const RECT& rect, BYTE alpha) {
        if (!hwnd || !bitmap.bitmap) return false;

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return false;

        HDC screen = GetDC(nullptr);
        HDC srcDC = CreateCompatibleDC(screen);
        HDC dstDC = CreateCompatibleDC(screen);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dstBmp = CreateDIBSection(
            screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

        HGDIOBJ oldSrc = SelectObject(srcDC, bitmap.bitmap);
        HGDIOBJ oldDst = SelectObject(dstDC, dstBmp);

        SetStretchBltMode(dstDC, HALFTONE);
        StretchBlt(dstDC, 0, 0, width, height, srcDC, 0, 0,
                   bitmap.width, bitmap.height, SRCCOPY);

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        blend.AlphaFormat = 0;

        POINT pos{rect.left, rect.top};
        SIZE size{width, height};
        POINT srcPos{0, 0};

        BOOL updated = UpdateLayeredWindow(
            hwnd, screen, &pos, &size, dstDC, &srcPos, 0, &blend, ULW_ALPHA);

        SelectObject(srcDC, oldSrc);
        SelectObject(dstDC, oldDst);
        DeleteObject(dstBmp);
        DeleteDC(srcDC);
        DeleteDC(dstDC);
        ReleaseDC(nullptr, screen);

        if (updated) {
            SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, width, height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        return updated != FALSE;
    }

    ~GhostWindow() {
        if (hwnd) DestroyWindow(hwnd);
    }
};

LRESULT CALLBACK GhostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void RunAnimation(HWND target,
                  Bitmap* bitmap,
                  RECT from,
                  RECT to,
                  int duration,
                  BYTE alphaStart,
                  BYTE alphaEnd,
                  bool closeWindow) {
    if (!bitmap) return;

    GhostWindow ghost;
    if (!ghost.Create()) {
        delete bitmap;
        FreeAnimationState(target);
        return;
    }

    const auto startTime = std::chrono::steady_clock::now();
    duration = std::clamp(duration, 50, 1000);

    while (!g_unloading && (closeWindow || IsWindow(target))) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double, std::milli>(now - startTime).count();
        const double linear = std::clamp(elapsed / duration, 0.0, 1.0);
        const double t = EaseOutCubic(linear);

        const RECT frame = InterpolateRect(from, to, t);
        const BYTE alpha = static_cast<BYTE>(
            std::clamp<int>(
                static_cast<int>(std::lround(
                    alphaStart + (alphaEnd - alphaStart) * t)),
                0, 255));

        if (!ghost.Render(*bitmap, frame, alpha)) break;

        if (linear >= 1.0) break;
        Sleep(7);
    }

    delete bitmap;

    if (!g_unloading) {
        if (!closeWindow && IsWindow(target)) {
            SetCloaked(target, false);
            SetDwmTransitions(target, true);
        }

        if (closeWindow) {
            RemovePropW(target, kBypassProp);
        }
    }

    FreeAnimationState(target);
}

void StartLaunchAnimation(HWND hwnd) {
    if (g_unloading || !g_settings.launchAnimation) return;
    if (!IsAnimatableWindow(hwnd) || IsExcluded(hwnd)) return;

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_activeWindows.count(hwnd)) return;
    }

    if (WasLaunchSeen(hwnd)) return;
    if (!MarkAnimationActive(hwnd)) return;

    SetDwmTransitions(hwnd, false);
    SetCloaked(hwnd, true);

    std::thread([hwnd] {
        TaskbarTarget taskbar = FindTaskbarButtonRetry(hwnd, 8, 50);
        Bitmap* bitmap = CaptureWindow(hwnd);

        if (!bitmap || !IsWindow(hwnd)) {
            delete bitmap;
            SetCloaked(hwnd, false);
            SetDwmTransitions(hwnd, true);
            FreeAnimationState(hwnd);
            return;
        }

        RECT targetRect = GetVisualWindowRect(hwnd);

        RECT iconRect{};
        if (taskbar.valid) {
            iconRect = taskbar.rect;
        } else {
            const int size = std::max(20, g_settings.iconSize);
            const POINT pt{targetRect.left + (targetRect.right - targetRect.left) / 2,
                           targetRect.bottom};
            iconRect = {pt.x - size / 2, pt.y - size / 2,
                        pt.x + (size + 1) / 2, pt.y + (size + 1) / 2};
        }

        RECT startRect = MakeStartRect(targetRect, iconRect);

        RunAnimation(hwnd, bitmap, startRect, targetRect,
                     g_settings.launchDuration,
                     static_cast<BYTE>(std::clamp(g_settings.startOpacity, 0, 100) * 255 / 100),
                     255, false);
    }).detach();
}

void StartCloseAnimation(HWND hwnd) {
    if (g_unloading || !g_settings.closeAnimation) return;
    if (!IsAnimatableWindow(hwnd) || IsExcluded(hwnd)) return;

    if (!MarkAnimationActive(hwnd)) return;

    Bitmap* bitmap = CaptureWindow(hwnd);
    if (!bitmap) {
        FreeAnimationState(hwnd);
        return;
    }

    const RECT targetRect = GetVisualWindowRect(hwnd);
    TaskbarTarget taskbar = FindTaskbarButtonRetry(hwnd, 4, 25);

    RECT iconRect{};
    if (taskbar.valid) {
        iconRect = taskbar.rect;
    } else {
        const int size = std::max(20, g_settings.iconSize);
        iconRect = {
            targetRect.left + (targetRect.right - targetRect.left) / 2 - size / 2,
            targetRect.bottom - size / 2,
            targetRect.left + (targetRect.right - targetRect.left) / 2 + (size + 1) / 2,
            targetRect.bottom + (size + 1) / 2
        };
    }

    const RECT endRect = MakeStartRect(targetRect, iconRect);

    SetDwmTransitions(hwnd, false);
    SetCloaked(hwnd, true);

    std::thread([hwnd, bitmap, targetRect, endRect] {
        // Close the real window while the ghost is covering it.
        SetPropW(hwnd, kBypassProp, reinterpret_cast<HANDLE>(1));
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

        RunAnimation(hwnd, bitmap, targetRect, endRect,
                     g_settings.closeDuration,
                     255,
                     static_cast<BYTE>(std::clamp(g_settings.closeEndOpacity, 0, 100) * 255 / 100),
                     true);
    }).detach();
}

void LoadSettings() {
    g_settings.launchAnimation = Wh_GetIntSetting(L"launch_animation") != 0;
    g_settings.closeAnimation = Wh_GetIntSetting(L"close_animation") != 0;

    g_settings.launchDuration = std::clamp(
        Wh_GetIntSetting(L"launch_duration"), 50, 1000);
    g_settings.closeDuration = std::clamp(
        Wh_GetIntSetting(L"close_duration"), 50, 1000);

    g_settings.startOpacity = std::clamp(
        Wh_GetIntSetting(L"start_opacity"), 0, 100);
    g_settings.closeEndOpacity = std::clamp(
        Wh_GetIntSetting(L"close_end_opacity"), 0, 100);

    g_settings.iconSize = std::clamp(
        Wh_GetIntSetting(L"icon_size"), 20, 64);

    g_settings.excluded.clear();

    for (int i = 0; i < 64; ++i) {
        PCWSTR value = Wh_GetStringSetting(L"excluded_processes[%d]", i);
        if (!value || !*value) {
            if (value) Wh_FreeStringSetting(value);
            break;
        }

        g_settings.excluded.push_back(ToLower(value));
        Wh_FreeStringSetting(value);
    }
}

} // namespace

BOOL WINAPI ShowWindow_Hook(HWND hwnd, int cmd) {
    const bool wasVisible = IsWindowVisible(hwnd) != FALSE;

    BOOL result = ShowWindow_Original(hwnd, cmd);

    if (!g_unloading &&
        g_settings.launchAnimation &&
        !wasVisible &&
        IsWindowVisible(hwnd) &&
        IsAnimatableWindow(hwnd) &&
        !IsExcluded(hwnd) &&
        (cmd == SW_SHOW || cmd == SW_SHOWNORMAL ||
         cmd == SW_SHOWNOACTIVATE || cmd == SW_RESTORE)) {

        StartLaunchAnimation(hwnd);
    }

    return result;
}

BOOL WINAPI ShowWindowAsync_Hook(HWND hwnd, int cmd) {
    const bool wasVisible = IsWindowVisible(hwnd) != FALSE;

    BOOL result = ShowWindowAsync_Original(hwnd, cmd);

    if (!g_unloading &&
        g_settings.launchAnimation &&
        !wasVisible &&
        IsWindowVisible(hwnd) &&
        IsAnimatableWindow(hwnd) &&
        !IsExcluded(hwnd) &&
        (cmd == SW_SHOW || cmd == SW_SHOWNORMAL ||
         cmd == SW_SHOWNOACTIVATE || cmd == SW_RESTORE)) {

        StartLaunchAnimation(hwnd);
    }

    return result;
}

LRESULT WINAPI DefWindowProcW_Hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_unloading &&
        g_settings.closeAnimation &&
        (msg == WM_CLOSE ||
         (msg == WM_SYSCOMMAND && (wp & 0xFFF0u) == SC_CLOSE)) &&
        IsWindow(hwnd) &&
        !IsExcluded(hwnd) &&
        !GetPropW(hwnd, kBypassProp)) {

        StartCloseAnimation(hwnd);
        return 0;
    }

    return DefWindowProcW_Original(hwnd, msg, wp, lp);
}

BOOL Wh_ModInit() {
    LoadSettings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GhostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Win11DockAnimationGhostWindow";

    g_ghostClass = RegisterClassExW(&wc);
    if (!g_ghostClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(ShowWindow),
            reinterpret_cast<void*>(ShowWindow_Hook),
            reinterpret_cast<void**>(&ShowWindow_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(ShowWindowAsync),
            reinterpret_cast<void*>(ShowWindowAsync_Hook),
            reinterpret_cast<void**>(&ShowWindowAsync_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DefWindowProcW),
            reinterpret_cast<void*>(DefWindowProcW_Hook),
            reinterpret_cast<void**>(&DefWindowProcW_Original))) {
        return FALSE;
    }

    Wh_Log(L"Win11 Window Icon Animation initialized");
    return TRUE;
}

void Wh_ModUninit() {
    g_unloading = true;

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_activeWindows.clear();
        g_seenLaunchWindows.clear();
    }

    Wh_Log(L"Win11 Window Icon Animation uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
