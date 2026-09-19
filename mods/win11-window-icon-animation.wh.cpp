// ==WindhawkMod==
// @id              win11-window-icon-animation
// @name            Windows 11 Genie Window Animation
// @description     Custom genie-style launch, restore, minimize and close animation.
// @version         0.5.0
// @author          cuuw619-maker
// @github          https://github.com/cuuw619-maker/Modpack
// @include         *
// @architecture    x86-64
// @compilerOptions -ldwmapi -lole32 -loleaut32 -luser32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 11 Genie Window Animation

The previous rectangle-zoom renderer has been removed.

The current renderer captures a window into a 32-bit DIB and deforms that
image row-by-row toward the real application Taskbar button. The inverse
deformation restores the window from the button.

Handled paths:
- application launch
- restore
- minimize
- close
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enable animation
- launch_animation: true
  $name: Animate launch
- restore_animation: true
  $name: Animate restore
- minimize_animation: true
  $name: Animate minimize
- close_animation: true
  $name: Animate close
- duration: 360
  $name: Duration (ms)
- spread: 35
  $name: Genie spread (%)
- neck_width: 10
  $name: Final neck width (%)
- curve: 8
  $name: Curve (%)
- fade_end: 10
  $name: Final opacity (%)
- excluded_processes: [""]
  $name: Excluded processes
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
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
    bool enabled = true;
    bool launchAnimation = true;
    bool restoreAnimation = true;
    bool minimizeAnimation = true;
    bool closeAnimation = true;
    int duration = 360;
    double spread = 0.35;
    double neckWidth = 0.10;
    double curve = 0.08;
    int fadeEnd = 10;
    std::vector<std::wstring> excluded;
};

Settings g_settings;
std::atomic<bool> g_unloading{false};
std::mutex g_stateMutex;
std::unordered_set<HWND> g_active;
std::unordered_set<HWND> g_launchSeen;

using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
using ShowWindowAsync_t = BOOL(WINAPI*)(HWND, int);
using SetWindowPlacement_t = BOOL(WINAPI*)(HWND, const WINDOWPLACEMENT*);
using CloseWindow_t = BOOL(WINAPI*)(HWND);
using DefWindowProcW_t = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

ShowWindow_t ShowWindow_Original = nullptr;
ShowWindowAsync_t ShowWindowAsync_Original = nullptr;
SetWindowPlacement_t SetWindowPlacement_Original = nullptr;
CloseWindow_t CloseWindow_Original = nullptr;
DefWindowProcW_t DefWindowProcW_Original = nullptr;
SetWindowPos_t SetWindowPos_Original = nullptr;

constexpr wchar_t kCloseBypass[] = L"Win11Genie.CloseBypass";
constexpr wchar_t kHiddenByUs[] = L"Win11Genie.HiddenByUs";
constexpr wchar_t kMinimizeBypass[] = L"Win11Genie.MinimizeBypass";

struct Frame {
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;

    ~Frame() {
        if (bitmap) {
            DeleteObject(bitmap);
        }
    }

    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

struct Target {
    RECT rect{};
    bool valid = false;
};

struct Job {
    HWND hwnd = nullptr;
    Frame* frame = nullptr;
    RECT windowRect{};
    RECT iconRect{};
    bool toDock = true;
    bool revealReal = false;
    bool uncloakAtEnd = false;
    bool finishClose = false;
    UINT closeMessage = WM_CLOSE;
    WPARAM closeWParam = 0;
    LPARAM closeLParam = 0;
    HANDLE firstFrame = nullptr;
};

void DisableNativeTransitions(HWND hwnd);
void EnableNativeTransitions(HWND hwnd);
void SignalFirstFrame(Job* job);

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) {
                       return static_cast<wchar_t>(towlower(c));
                   });
    return value;
}

bool IsExcluded(HWND hwnd) {
    if (!hwnd || g_settings.excluded.empty()) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    wchar_t path[MAX_PATH]{};
    DWORD length = ARRAYSIZE(path);
    bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);

    if (!ok) {
        return false;
    }

    const std::wstring full = Lower(path);
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring base =
        slash == std::wstring::npos ? full : full.substr(slash + 1);

    for (const auto& item : g_settings.excluded) {
        if (item == full || item == base) {
            return true;
        }
    }

    return false;
}

bool IsAnimatableWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) ||
        GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if ((style & WS_CHILD) ||
        (exStyle & WS_EX_TOOLWINDOW)) {
        return false;
    }

    RECT rect{};

    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(hwnd, &placement)) {
            return false;
        }
        rect = placement.rcNormalPosition;
    } else if (!GetWindowRect(hwnd, &rect)) {
        return false;
    }

    return rect.right - rect.left >= 80 &&
           rect.bottom - rect.top >= 60;
}

RECT GetFrameRect(HWND hwnd) {
    RECT rect{};

    if (FAILED(DwmGetWindowAttribute(
            hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
            &rect, sizeof(rect)))) {
        GetWindowRect(hwnd, &rect);
    }

    return rect;
}

void SetDwmTransitions(HWND hwnd, bool enabled) {
    BOOL disabled = enabled ? FALSE : TRUE;
    DwmSetWindowAttribute(
        hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disabled, sizeof(disabled));
}

void SetCloak(HWND hwnd, bool value) {
    BOOL cloak = value ? TRUE : FALSE;
    DwmSetWindowAttribute(
        hwnd, DWMWA_CLOAK,
        &cloak, sizeof(cloak));
}

void DisableNativeTransitions(HWND hwnd) {
    SetDwmTransitions(hwnd, false);
}

void EnableNativeTransitions(HWND hwnd) {
    SetDwmTransitions(hwnd, true);
}

void SignalFirstFrame(Job* job) {
    if (job && job->firstFrame) {
        SetEvent(job->firstFrame);
    }
}

void EnsureLayered(HWND hwnd) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongPtrW(
            hwnd, GWL_EXSTYLE,
            ex | WS_EX_LAYERED);
    }
}

void SetAlpha(HWND hwnd, BYTE alpha) {
    EnsureLayered(hwnd);

    SetLayeredWindowAttributes(
        hwnd, 0, alpha, LWA_ALPHA);
}

bool MarkActive(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_active.insert(hwnd).second;
}

void ClearActive(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_active.erase(hwnd);
}

bool MarkLaunchSeen(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_launchSeen.insert(hwnd).second;
}

Frame* CaptureWindow(HWND hwnd) {
    const RECT rect = GetFrameRect(hwnd);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (width < 2 || height < 2) {
        return nullptr;
    }

    auto* frame = new Frame();
    frame->width = width;
    frame->height = height;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        delete frame;
        return nullptr;
    }

    HDC dc = CreateCompatibleDC(screen);
    if (!dc) {
        ReleaseDC(nullptr, screen);
        delete frame;
        return nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    frame->bitmap = CreateDIBSection(
        screen, &bmi, DIB_RGB_COLORS,
        &frame->bits, nullptr, 0);

    if (!frame->bitmap) {
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        delete frame;
        return nullptr;
    }

    HGDIOBJ old = SelectObject(dc, frame->bitmap);

    BOOL painted =
        PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);

    if (!painted) {
        painted = PrintWindow(hwnd, dc, 0);
    }

    if (!painted) {
        RECT raw{};
        GetWindowRect(hwnd, &raw);
        painted = BitBlt(
            dc, 0, 0, width, height, screen,
            raw.left, raw.top,
            SRCCOPY | CAPTUREBLT);
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    if (!painted) {
        delete frame;
        return nullptr;
    }

    DWORD* pixels =
        static_cast<DWORD*>(frame->bits);

    const size_t count =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    for (size_t i = 0; i < count; ++i) {
        pixels[i] |= 0xFF000000u;
    }

    return frame;
}

HWND FindTaskbarForMonitor(HMONITOR monitor) {
    HWND primary =
        FindWindowW(L"Shell_TrayWnd", nullptr);

    if (!monitor) {
        return primary;
    }

    if (primary &&
        MonitorFromWindow(
            primary, MONITOR_DEFAULTTONULL) == monitor) {
        return primary;
    }

    HWND secondary = nullptr;

    while ((secondary = FindWindowExW(
                nullptr, secondary,
                L"Shell_SecondaryTrayWnd",
                nullptr)) != nullptr) {

        if (MonitorFromWindow(
                secondary,
                MONITOR_DEFAULTTONULL) == monitor) {
            return secondary;
        }
    }

    return primary;
}

Target FindTaskbarButton(HWND hwnd) {
    Target result{};

    HWND taskbar =
        FindTaskbarForMonitor(
            MonitorFromWindow(
                hwnd,
                MONITOR_DEFAULTTONEAREST));

    if (!taskbar) {
        return result;
    }

    HRESULT init =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);

    const bool ownApartment =
        init == S_OK || init == S_FALSE;

    IUIAutomation* automation = nullptr;

    HRESULT hr =
        CoCreateInstance(
            __uuidof(CUIAutomation8),
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation),
            reinterpret_cast<void**>(&automation));

    if (FAILED(hr)) {
        hr =
            CoCreateInstance(
                __uuidof(CUIAutomation),
                nullptr,
                CLSCTX_INPROC_SERVER,
                __uuidof(IUIAutomation),
                reinterpret_cast<void**>(&automation));
    }

    if (FAILED(hr) || !automation) {
        if (ownApartment) {
            CoUninitialize();
        }
        return result;
    }

    IUIAutomationElement* tray = nullptr;

    if (SUCCEEDED(
            automation->ElementFromHandle(
                taskbar,
                &tray)) &&
        tray) {

        VARIANT buttonType{};
        buttonType.vt = VT_I4;
        buttonType.lVal =
            UIA_ButtonControlTypeId;

        VARIANT listType{};
        listType.vt = VT_I4;
        listType.lVal =
            UIA_ListItemControlTypeId;

        IUIAutomationCondition* buttonCond = nullptr;
        IUIAutomationCondition* listCond = nullptr;
        IUIAutomationCondition* orCond = nullptr;

        automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId,
            buttonType,
            &buttonCond);

        automation->CreatePropertyCondition(
            UIA_ControlTypePropertyId,
            listType,
            &listCond);

        if (buttonCond && listCond) {
            automation->CreateOrCondition(
                buttonCond,
                listCond,
                &orCond);
        }

        IUIAutomationElementArray* elements = nullptr;

        if (orCond &&
            SUCCEEDED(
                tray->FindAll(
                    TreeScope_Descendants,
                    orCond,
                    &elements)) &&
            elements) {

            wchar_t titleBuffer[512]{};
            GetWindowTextW(
                hwnd,
                titleBuffer,
                ARRAYSIZE(titleBuffer));

            const std::wstring title =
                Lower(titleBuffer);

            DWORD pid = 0;
            GetWindowThreadProcessId(
                hwnd,
                &pid);

            std::wstring processName;

            HANDLE process =
                OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE,
                    pid);

            if (process) {
                wchar_t path[MAX_PATH]{};
                DWORD length =
                    ARRAYSIZE(path);

                if (QueryFullProcessImageNameW(
                        process,
                        0,
                        path,
                        &length)) {

                    std::wstring full =
                        Lower(path);

                    size_t slash =
                        full.find_last_of(
                            L"\\/");

                    processName =
                        slash == std::wstring::npos
                            ? full
                            : full.substr(
                                  slash + 1);

                    size_t dot =
                        processName.rfind(L'.');

                    if (dot !=
                        std::wstring::npos) {
                        processName.resize(dot);
                    }
                }

                CloseHandle(process);
            }

            int bestScore = -100000;
            int length = 0;
            elements->get_Length(&length);

            for (int i = 0;
                 i < length;
                 ++i) {

                IUIAutomationElement* element =
                    nullptr;

                if (FAILED(
                        elements->GetElement(
                            i,
                            &element)) ||
                    !element) {
                    continue;
                }

                BSTR bstr = nullptr;

                if (SUCCEEDED(
                        element->get_CurrentName(
                            &bstr)) &&
                    bstr) {

                    std::wstring name =
                        Lower(bstr);

                    SysFreeString(bstr);

                    int score = 0;

                    if (!title.empty() &&
                        name == title) {
                        score += 1200;
                    }

                    if (!title.empty() &&
                        (name.find(title) !=
                             std::wstring::npos ||
                         title.find(name) !=
                             std::wstring::npos)) {
                        score += 450;
                    }

                    if (!processName.empty() &&
                        name.find(processName) !=
                            std::wstring::npos) {
                        score += 750;
                    }

                    if (name.find(L"start") !=
                        std::wstring::npos) {
                        score -= 800;
                    }

                    if (name.find(L"search") !=
                        std::wstring::npos) {
                        score -= 800;
                    }

                    if (name.find(L"task view") !=
                        std::wstring::npos) {
                        score -= 800;
                    }

                    RECT bounds{};

                    if (score > bestScore &&
                        SUCCEEDED(
                            element->
                                get_CurrentBoundingRectangle(
                                    &bounds)) &&
                        bounds.right > bounds.left &&
                        bounds.bottom > bounds.top) {

                        bestScore = score;
                        result.rect = bounds;
                        result.valid = true;
                    }
                }

                element->Release();
            }

            elements->Release();
        }

        if (buttonCond) {
            buttonCond->Release();
        }

        if (listCond) {
            listCond->Release();
        }

        if (orCond) {
            orCond->Release();
        }

        tray->Release();
    }

    automation->Release();

    if (ownApartment) {
        CoUninitialize();
    }

    return result;
}

RECT GetIconRect(
    HWND hwnd,
    const RECT& windowRect) {

    for (int i = 0;
         i < 8 && !g_unloading;
         ++i) {

        Target target =
            FindTaskbarButton(hwnd);

        if (target.valid) {
            return target.rect;
        }

        Sleep(20);
    }

    HWND taskbar =
        FindTaskbarForMonitor(
            MonitorFromRect(
                &windowRect,
                MONITOR_DEFAULTTONEAREST));

    const int size = 34;

    if (taskbar) {
        RECT tray{};

        if (GetWindowRect(
                taskbar,
                &tray)) {

            const int cx =
                (windowRect.left +
                 windowRect.right) /
                2;

            const int cy =
                (tray.top +
                 tray.bottom) /
                2;

            return {
                cx - size / 2,
                cy - size / 2,
                cx + (size + 1) / 2,
                cy + (size + 1) / 2
            };
        }
    }

    const int cx =
        (windowRect.left +
         windowRect.right) /
        2;

    const int cy =
        windowRect.bottom;

    return {
        cx - size / 2,
        cy - size / 2,
        cx + (size + 1) / 2,
        cy + (size + 1) / 2
    };
}

double Smooth(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double RowPhase(
    double progress,
    double sourceV,
    bool toDock) {

    double fromDock =
        toDock
            ? (1.0 - sourceV)
            : sourceV;

    double delay =
        fromDock *
        g_settings.spread;

    double local =
        (progress - delay) /
        std::max(0.001, 1.0 - delay);

    return Smooth(local);
}

bool RenderGenie(
    const Frame& source,
    const RECT& windowRect,
    const RECT& iconRect,
    double progress,
    bool toDock,
    HWND ghost,
    int left,
    int top,
    int canvasWidth,
    int canvasHeight,
    HDC screen,
    HDC canvasDC,
    void* canvasBits) {

    const int width = source.width;
    const int height = source.height;

    DWORD* dst =
        static_cast<DWORD*>(canvasBits);

    std::fill_n(
        dst,
        static_cast<size_t>(canvasWidth) *
        static_cast<size_t>(canvasHeight),
        0u);

    const BYTE* src =
        static_cast<const BYTE*>(
            source.bits);

    const int srcStride =
        width * 4;

    const int dstStride =
        canvasWidth * 4;

    const double centerX =
        (windowRect.left +
         windowRect.right) *
        0.5;

    const double targetX =
        (iconRect.left +
         iconRect.right) *
        0.5;

    const double iconTop =
        static_cast<double>(
            iconRect.top);

    const double iconHeight =
        static_cast<double>(
            std::max(
                4L,
                iconRect.bottom -
                iconRect.top));

    const double iconWidth =
        static_cast<double>(
            std::max(
                4L,
                iconRect.right -
                iconRect.left));

    const double neck =
        std::max(
            2.0,
            std::min(
                static_cast<double>(width),
                static_cast<double>(height)) *
            g_settings.neckWidth);

    std::vector<float> rowMap(
        static_cast<size_t>(
            height) + 1);

    for (int y = 0;
         y <= height;
         ++y) {

        const double v =
            static_cast<double>(y) /
            static_cast<double>(height);

        const double phase =
            RowPhase(
                progress,
                v,
                toDock);

        const double sourceY =
            windowRect.top +
            height * v;

        const double destinationY =
            iconTop +
            iconHeight * v;

        rowMap[
            static_cast<size_t>(y)] =
            static_cast<float>(
                sourceY +
                (destinationY -
                 sourceY) *
                phase);
    }

    for (int y = 1;
         y <= height;
         ++y) {

        float& current =
            rowMap[
                static_cast<size_t>(y)];

        const float previous =
            rowMap[
                static_cast<size_t>(
                    y - 1)];

        if (current <= previous) {
            current =
                previous + 0.01f;
        }
    }

    int segment = 0;

    for (int canvasY = 0;
         canvasY < canvasHeight;
         ++canvasY) {

        const double screenY =
            top + canvasY + 0.5;

        if (screenY < rowMap[0] ||
            screenY >=
                rowMap[
                    static_cast<size_t>(
                        height)]) {
            continue;
        }

        while (
            segment < height - 1 &&
            rowMap[
                static_cast<size_t>(
                    segment + 1)] <=
                screenY) {

            ++segment;
        }

        const double y0 =
            rowMap[
                static_cast<size_t>(
                    segment)];

        const double y1 =
            rowMap[
                static_cast<size_t>(
                    segment + 1)];

        const double fraction =
            std::clamp(
                (screenY - y0) /
                std::max(
                    0.001,
                    y1 - y0),
                0.0,
                1.0);

        const double sourceV =
            (segment + fraction) /
            static_cast<double>(height);

        const double phase =
            RowPhase(
                progress,
                sourceV,
                toDock);

        double rowWidth =
            width +
            (iconWidth - width) *
            phase;

        if (toDock &&
            progress > 0.84) {

            const double tail =
                Smooth(
                    (progress - 0.84) /
                    0.16);

            rowWidth =
                rowWidth *
                    (1.0 -
                     0.72 * tail) +
                neck *
                    (0.72 * tail);
        }

        if (!toDock &&
            progress < 0.16) {

            const double head =
                Smooth(
                    (0.16 -
                     progress) /
                    0.16);

            rowWidth =
                rowWidth *
                    (1.0 -
                     0.72 * head) +
                neck *
                    (0.72 * head);
        }

        rowWidth =
            std::clamp(
                rowWidth,
                1.0,
                static_cast<double>(
                    width) * 1.5);

        double x =
            centerX +
            (targetX - centerX) *
            phase;

        const double bend =
            std::sin(
                sourceV *
                3.141592653589793) *
            std::max(
                static_cast<double>(width),
                static_cast<double>(height)) *
            g_settings.curve *
            phase;

        if (targetX >= centerX) {
            x += toDock ? bend : -bend;
        } else {
            x += toDock ? -bend : bend;
        }

        int rowLeft =
            static_cast<int>(
                std::floor(
                    x -
                    rowWidth * 0.5));

        int rowRight =
            static_cast<int>(
                std::ceil(
                    x +
                    rowWidth * 0.5));

        rowLeft =
            std::max(
                rowLeft,
                left);

        rowRight =
            std::min(
                rowRight,
                left + canvasWidth);

        if (rowRight <= rowLeft) {
            continue;
        }

        int sourceRow =
            static_cast<int>(
                std::floor(
                    sourceV *
                    (height - 1)));

        sourceRow =
            std::clamp(
                sourceRow,
                0,
                height - 1);

        const BYTE* srcRow =
            src +
            static_cast<size_t>(
                sourceRow) *
            static_cast<size_t>(
                srcStride);

        BYTE* dstRow =
            reinterpret_cast<BYTE*>(
                dst) +
            static_cast<size_t>(
                canvasY) *
            static_cast<size_t>(
                dstStride);

        for (int px = rowLeft;
             px < rowRight;
             ++px) {

            const double u =
                ((px - x) /
                 std::max(
                     1.0,
                     rowWidth)) +
                0.5;

            if (u < 0.0 ||
                u >= 1.0) {
                continue;
            }

            int sourceX =
                static_cast<int>(
                    std::floor(
                        u * width));

            sourceX =
                std::clamp(
                    sourceX,
                    0,
                    width - 1);

            const BYTE* sp =
                srcRow +
                static_cast<size_t>(
                    sourceX) * 4;

            BYTE* dp =
                dstRow +
                static_cast<size_t>(
                    px - left) * 4;

            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];

            if (toDock &&
                progress > 0.80) {

                const double fade =
                    Smooth(
                        (progress - 0.80) /
                        0.20);

                const double opacity =
                    1.0 -
                    (1.0 -
                     g_settings.fadeEnd /
                         100.0) *
                    fade;

                dp[3] =
                    static_cast<BYTE>(
                        dp[3] *
                        std::clamp(
                            opacity,
                            0.0,
                            1.0));
            }
        }
    }

    POINT dstPoint{left, top};
    SIZE size{canvasWidth, canvasHeight};
    POINT srcPoint{0, 0};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    return UpdateLayeredWindow(
               ghost,
               screen,
               &dstPoint,
               &size,
               canvasDC,
               &srcPoint,
               0,
               &blend,
               ULW_ALPHA) != FALSE;
}

void RunJob(Job* job) {
    if (!job || !job->frame) {
        if (job) {
            if (job->firstFrame) {
                CloseHandle(job->firstFrame);
            }
            delete job;
        }
        return;
    }

    Frame* frame = job->frame;

    int left =
        std::min(
            job->windowRect.left,
            job->iconRect.left);

    int right =
        std::max(
            job->windowRect.right,
            job->iconRect.right);

    int top =
        std::min(
            job->windowRect.top,
            job->iconRect.top);

    int bottom =
        std::max(
            job->windowRect.bottom,
            job->iconRect.bottom);

    int pad =
        static_cast<int>(
            std::max(
                24.0,
                std::max(
                    std::abs(
                        job->windowRect.left -
                        job->iconRect.left),
                    std::abs(
                        job->windowRect.right -
                        job->iconRect.right)) *
                    0.10));

    left -= pad;
    right += pad;
    top -= 18;
    bottom += 18;

    HDC screen = GetDC(nullptr);
    HDC canvasDC =
        screen
            ? CreateCompatibleDC(screen)
            : nullptr;

    if (!screen || !canvasDC) {
        if (canvasDC) {
            DeleteDC(canvasDC);
        }
        if (screen) {
            ReleaseDC(nullptr, screen);
        }
        delete frame;
        SignalFirstFrame(job);
        if (job->firstFrame) {
            CloseHandle(job->firstFrame);
        }
        if (job->uncloakAtEnd && IsWindow(job->hwnd)) {
            SetCloak(job->hwnd, false);
            EnableNativeTransitions(job->hwnd);
        }
        ClearActive(job->hwnd);
        delete job;
        return;
    }

    const int canvasWidth =
        std::max(2, right - left);

    const int canvasHeight =
        std::max(2, bottom - top);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth =
        canvasWidth;
    bmi.bmiHeader.biHeight =
        -canvasHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression =
        BI_RGB;

    void* canvasBits = nullptr;

    HBITMAP canvas =
        CreateDIBSection(
            screen,
            &bmi,
            DIB_RGB_COLORS,
            &canvasBits,
            nullptr,
            0);

    if (!canvas || !canvasBits) {
        if (canvas) {
            DeleteObject(canvas);
        }
        DeleteDC(canvasDC);
        ReleaseDC(nullptr, screen);
        delete frame;
        SignalFirstFrame(job);
        if (job->firstFrame) {
            CloseHandle(job->firstFrame);
        }
        if (job->uncloakAtEnd && IsWindow(job->hwnd)) {
            SetCloak(job->hwnd, false);
            EnableNativeTransitions(job->hwnd);
        }
        ClearActive(job->hwnd);
        delete job;
        return;
    }

    HGDIOBJ old =
        SelectObject(
            canvasDC,
            canvas);

    HWND ghost =
        CreateWindowExW(
            WS_EX_LAYERED |
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST |
            WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE,
            L"STATIC",
            L"Win11GenieGhost",
            WS_POPUP,
            left,
            top,
            canvasWidth,
            canvasHeight,
            nullptr,
            nullptr,
            nullptr,
            nullptr);

    if (!ghost) {
        SelectObject(
            canvasDC,
            old);
        DeleteObject(canvas);
        DeleteDC(canvasDC);
        ReleaseDC(nullptr, screen);
        delete frame;
        SignalFirstFrame(job);
        if (job->firstFrame) {
            CloseHandle(job->firstFrame);
        }
        if (job->uncloakAtEnd && IsWindow(job->hwnd)) {
            SetCloak(job->hwnd, false);
            EnableNativeTransitions(job->hwnd);
        }
        ClearActive(job->hwnd);
        delete job;
        return;
    }

    // Explicitly show the layered popup before presenting frames.
    // Keep it non-activating so it cannot steal focus from the real window.
    ShowWindow(ghost, SW_SHOWNOACTIVATE);
    SetWindowPos(
        ghost,
        HWND_TOPMOST,
        left,
        top,
        canvasWidth,
        canvasHeight,
        SWP_NOACTIVATE |
        SWP_SHOWWINDOW);

    bool firstPresented = false;
    const ULONGLONG started =
        GetTickCount64();

    for (;;) {
        if (g_unloading.load(
                std::memory_order_relaxed)) {
            break;
        }

        const ULONGLONG now =
            GetTickCount64();

        double progress =
            (now - started) /
            static_cast<double>(
                std::max(
                    80,
                    g_settings.duration));

        progress =
            std::clamp(
                progress,
                0.0,
                1.0);

        const double eased =
            Smooth(progress);

        const double morph =
            job->toDock
                ? eased
                : 1.0 - eased;

        if (!RenderGenie(
                *frame,
                job->windowRect,
                job->iconRect,
                morph,
                job->toDock,
                ghost,
                left,
                top,
                canvasWidth,
                canvasHeight,
                screen,
                canvasDC,
                canvasBits)) {
            break;
        }

        if (!firstPresented) {
            firstPresented = true;
            SignalFirstFrame(job);
            DwmFlush();
        }

        if (progress >= 1.0) {
            break;
        }

        Sleep(6);
    }

    if (!g_unloading.load(
            std::memory_order_relaxed)) {

        if (job->revealReal &&
            IsWindow(job->hwnd)) {

            SetCloak(
                job->hwnd,
                false);

            if (GetPropW(
                    job->hwnd,
                    kHiddenByUs)) {

                SetAlpha(
                    job->hwnd,
                    255);

                RemovePropW(
                    job->hwnd,
                    kHiddenByUs);
            }

            EnableNativeTransitions(
                job->hwnd);

        } else if (job->uncloakAtEnd &&
                   IsWindow(job->hwnd)) {

            // Minimize path: the real HWND stayed cloaked while the
            // overlay performed the deformation. Release the cloak only
            // after the custom animation has finished.
            SetCloak(
                job->hwnd,
                false);

            EnableNativeTransitions(
                job->hwnd);
        }

        if (job->finishClose &&
            IsWindow(job->hwnd)) {

            SetPropW(
                job->hwnd,
                kCloseBypass,
                reinterpret_cast<HANDLE>(1));

            if (job->closeMessage ==
                WM_SYSCOMMAND) {

                PostMessageW(
                    job->hwnd,
                    WM_SYSCOMMAND,
                    job->closeWParam,
                    job->closeLParam);

            } else {

                PostMessageW(
                    job->hwnd,
                    WM_CLOSE,
                    job->closeWParam,
                    job->closeLParam);
            }
        }

    } else if (job->revealReal &&
               IsWindow(job->hwnd)) {

        SetCloak(
            job->hwnd,
            false);

        SetAlpha(
            job->hwnd,
            255);

        EnableNativeTransitions(
            job->hwnd);
    }

    SelectObject(
        canvasDC,
        old);

    DeleteObject(canvas);
    DeleteDC(canvasDC);
    ReleaseDC(nullptr, screen);
    DestroyWindow(ghost);

    delete frame;
    ClearActive(job->hwnd);
    if (job->firstFrame) {
        CloseHandle(job->firstFrame);
    }
    delete job;
}

bool StartJob(Job* job) {
    if (!job ||
        !job->frame ||
        g_unloading.load(
            std::memory_order_relaxed)) {

        if (job) {
            delete job->frame;
            if (job->firstFrame) {
                CloseHandle(
                    job->firstFrame);
            }
            delete job;
        }

        return false;
    }

    try {
        std::thread(
            [job] {
                RunJob(job);
            }).detach();
    } catch (...) {
        delete job->frame;
        if (job->firstFrame) {
            CloseHandle(job->firstFrame);
        }
        delete job;
        return false;
    }

    return true;
}

bool BeginMinimize(HWND hwnd) {
    if (g_unloading ||
        !g_settings.enabled ||
        !g_settings.minimizeAnimation ||
        IsExcluded(hwnd) ||
        !IsAnimatableWindow(hwnd) ||
        IsIconic(hwnd)) {
        return false;
    }

    if (!MarkActive(hwnd)) {
        return false;
    }

    Frame* frame =
        CaptureWindow(hwnd);

    if (!frame) {
        ClearActive(hwnd);
        return false;
    }

    const RECT windowRect =
        GetFrameRect(hwnd);

    const RECT iconRect =
        GetIconRect(
            hwnd,
            windowRect);

    // Hide the real window before Windows gets a chance to animate it.
    // The only visible representation during minimize is our layered genie.
    DisableNativeTransitions(hwnd);
    SetCloak(hwnd, true);

    Job* job =
        new Job();

    job->hwnd = hwnd;
    job->frame = frame;
    job->windowRect = windowRect;
    job->iconRect = iconRect;
    job->toDock = true;
    job->uncloakAtEnd = true;
    job->firstFrame =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr);

    HANDLE first = job->firstFrame;

    if (!StartJob(job)) {
        SetCloak(hwnd, false);
        EnableNativeTransitions(hwnd);
        return false;
    }

    // The real window is already cloaked, so there is no reason to delay
    // the original minimize call. This prevents the native transition from
    // competing with the custom overlay.
    return true;
}

bool BeginRestore(
    HWND hwnd,
    int command) {

    if (g_unloading ||
        !g_settings.enabled ||
        !g_settings.restoreAnimation ||
        IsExcluded(hwnd) ||
        !IsAnimatableWindow(hwnd) ||
        !IsIconic(hwnd)) {
        return false;
    }

    if (!MarkActive(hwnd)) {
        return false;
    }

    DisableNativeTransitions(hwnd);
    SetCloak(hwnd, true);

    ShowWindow_Original(
        hwnd,
        command);

    Sleep(12);

    Frame* frame =
        CaptureWindow(hwnd);

    if (!frame) {
        SetCloak(hwnd, false);
        EnableNativeTransitions(hwnd);
        ClearActive(hwnd);
        return false;
    }

    const RECT windowRect =
        GetFrameRect(hwnd);

    const RECT iconRect =
        GetIconRect(
            hwnd,
            windowRect);

    SetPropW(
        hwnd,
        kHiddenByUs,
        reinterpret_cast<HANDLE>(1));

    Job* job = new Job();

    job->hwnd = hwnd;
    job->frame = frame;
    job->windowRect = windowRect;
    job->iconRect = iconRect;
    job->toDock = false;
    job->revealReal = true;

    if (!StartJob(job)) {
        RemovePropW(
            hwnd,
            kHiddenByUs);
        SetCloak(hwnd, false);
        EnableNativeTransitions(hwnd);
        ClearActive(hwnd);
        return false;
    }

    return true;
}

bool BeginLaunch(
    HWND hwnd,
    int command) {

    if (g_unloading ||
        !g_settings.enabled ||
        !g_settings.launchAnimation ||
        IsExcluded(hwnd) ||
        !IsAnimatableWindow(hwnd) ||
        IsWindowVisible(hwnd) ||
        IsIconic(hwnd)) {
        return false;
    }

    if (!MarkLaunchSeen(hwnd) ||
        !MarkActive(hwnd)) {
        return false;
    }

    DisableNativeTransitions(hwnd);
    SetAlpha(hwnd, 0);

    ShowWindow_Original(
        hwnd,
        command);

    Sleep(20);

    Frame* frame =
        CaptureWindow(hwnd);

    if (!frame) {
        SetAlpha(hwnd, 255);
        EnableNativeTransitions(hwnd);
        ClearActive(hwnd);
        return false;
    }

    const RECT windowRect =
        GetFrameRect(hwnd);

    const RECT iconRect =
        GetIconRect(
            hwnd,
            windowRect);

    SetPropW(
        hwnd,
        kHiddenByUs,
        reinterpret_cast<HANDLE>(1));

    Job* job = new Job();

    job->hwnd = hwnd;
    job->frame = frame;
    job->windowRect = windowRect;
    job->iconRect = iconRect;
    job->toDock = false;
    job->revealReal = true;

    if (!StartJob(job)) {
        RemovePropW(
            hwnd,
            kHiddenByUs);
        SetAlpha(hwnd, 255);
        EnableNativeTransitions(hwnd);
        ClearActive(hwnd);
        return false;
    }

    return true;
}

bool BeginClose(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    if (g_unloading ||
        !g_settings.enabled ||
        !g_settings.closeAnimation ||
        IsExcluded(hwnd) ||
        !IsAnimatableWindow(hwnd) ||
        GetPropW(hwnd, kCloseBypass)) {
        return false;
    }

    if (!MarkActive(hwnd)) {
        return false;
    }

    Frame* frame =
        CaptureWindow(hwnd);

    if (!frame) {
        ClearActive(hwnd);
        return false;
    }

    const RECT windowRect =
        GetFrameRect(hwnd);

    const RECT iconRect =
        GetIconRect(
            hwnd,
            windowRect);

    DisableNativeTransitions(hwnd);
    SetCloak(hwnd, true);

    Job* job = new Job();

    job->hwnd = hwnd;
    job->frame = frame;
    job->windowRect = windowRect;
    job->iconRect = iconRect;
    job->toDock = true;
    job->finishClose = true;
    job->closeMessage = message;
    job->closeWParam = wParam;
    job->closeLParam = lParam;

    if (!StartJob(job)) {
        SetCloak(hwnd, false);
        EnableNativeTransitions(hwnd);
        ClearActive(hwnd);
        return false;
    }

    return true;
}

void LoadSettings() {
    g_settings.enabled =
        Wh_GetIntSetting(L"enabled") != 0;

    g_settings.launchAnimation =
        Wh_GetIntSetting(L"launch_animation") != 0;

    g_settings.restoreAnimation =
        Wh_GetIntSetting(L"restore_animation") != 0;

    g_settings.minimizeAnimation =
        Wh_GetIntSetting(L"minimize_animation") != 0;

    g_settings.closeAnimation =
        Wh_GetIntSetting(L"close_animation") != 0;

    g_settings.duration =
        std::clamp(
            Wh_GetIntSetting(L"duration"),
            80,
            1200);

    g_settings.spread =
        std::clamp(
            Wh_GetIntSetting(L"spread"),
            5,
            80) /
        100.0;

    g_settings.neckWidth =
        std::clamp(
            Wh_GetIntSetting(L"neck_width"),
            3,
            35) /
        100.0;

    g_settings.curve =
        std::clamp(
            Wh_GetIntSetting(L"curve"),
            0,
            20) /
        100.0;

    g_settings.fadeEnd =
        std::clamp(
            Wh_GetIntSetting(L"fade_end"),
            0,
            100);

    std::vector<std::wstring> excluded;

    for (int i = 0;
         i < 64;
         ++i) {

        PCWSTR value =
            Wh_GetStringSetting(
                L"excluded_processes[%d]",
                i);

        if (!value ||
            !*value) {

            if (value) {
                Wh_FreeStringSetting(value);
            }

            break;
        }

        excluded.push_back(
            Lower(value));

        Wh_FreeStringSetting(value);
    }

    g_settings.excluded =
        std::move(excluded);
}

BOOL WINAPI ShowWindow_Hook(
    HWND hwnd,
    int command) {

    if (g_unloading) {
        return ShowWindow_Original(
            hwnd,
            command);
    }

    if (command == SW_MINIMIZE ||
        command == SW_SHOWMINIMIZED ||
        command == SW_SHOWMINNOACTIVE) {

        if (GetPropW(
                hwnd,
                kMinimizeBypass)) {

            RemovePropW(
                hwnd,
                kMinimizeBypass);

            return ShowWindow_Original(
                hwnd,
                command);
        }

        if (BeginMinimize(hwnd)) {
            return ShowWindow_Original(
                hwnd,
                command);
        }
    }

    if ((command == SW_RESTORE ||
         command == SW_SHOWNORMAL) &&
        IsIconic(hwnd)) {

        if (BeginRestore(
                hwnd,
                command)) {
            return TRUE;
        }
    }

    if (!IsWindowVisible(hwnd) &&
        !IsIconic(hwnd) &&
        (command == SW_SHOW ||
         command == SW_SHOWNORMAL ||
         command == SW_SHOWDEFAULT ||
         command == SW_SHOWMAXIMIZED)) {

        if (BeginLaunch(
                hwnd,
                command)) {
            return TRUE;
        }
    }

    return ShowWindow_Original(
        hwnd,
        command);
}

BOOL WINAPI ShowWindowAsync_Hook(
    HWND hwnd,
    int command) {

    if (g_unloading) {
        return ShowWindowAsync_Original(
            hwnd,
            command);
    }

    if (command == SW_MINIMIZE ||
        command == SW_SHOWMINIMIZED ||
        command == SW_SHOWMINNOACTIVE) {

        if (GetPropW(
                hwnd,
                kMinimizeBypass)) {

            RemovePropW(
                hwnd,
                kMinimizeBypass);

            return ShowWindowAsync_Original(
                hwnd,
                command);
        }

        if (BeginMinimize(hwnd)) {
            return ShowWindowAsync_Original(
                hwnd,
                command);
        }
    }

    if ((command == SW_RESTORE ||
         command == SW_SHOWNORMAL) &&
        IsIconic(hwnd)) {

        if (BeginRestore(
                hwnd,
                command)) {
            return TRUE;
        }
    }

    if (!IsWindowVisible(hwnd) &&
        !IsIconic(hwnd) &&
        (command == SW_SHOW ||
         command == SW_SHOWNORMAL ||
         command == SW_SHOWDEFAULT ||
         command == SW_SHOWMAXIMIZED)) {

        if (BeginLaunch(
                hwnd,
                command)) {
            return TRUE;
        }
    }

    return ShowWindowAsync_Original(
        hwnd,
        command);
}

BOOL WINAPI SetWindowPlacement_Hook(
    HWND hwnd,
    const WINDOWPLACEMENT* placement) {

    if (placement &&
        (placement->showCmd ==
             SW_MINIMIZE ||
         placement->showCmd ==
             SW_SHOWMINIMIZED ||
         placement->showCmd ==
             SW_SHOWMINNOACTIVE)) {

        if (BeginMinimize(hwnd)) {
            SetPropW(
                hwnd,
                kMinimizeBypass,
                reinterpret_cast<HANDLE>(1));

            BOOL result =
                SetWindowPlacement_Original(
                    hwnd,
                    placement);

            RemovePropW(
                hwnd,
                kMinimizeBypass);

            return result;
        }
    }

    return SetWindowPlacement_Original(
        hwnd,
        placement);
}

BOOL WINAPI CloseWindow_Hook(HWND hwnd) {
    if (BeginClose(
            hwnd,
            WM_CLOSE,
            0,
            0)) {
        return TRUE;
    }

    return CloseWindow_Original(hwnd);
}

LRESULT WINAPI DefWindowProcW_Hook(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    if (message == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(
            g_stateMutex);

        g_launchSeen.erase(hwnd);
        g_active.erase(hwnd);
    }

    if (message == WM_SYSCOMMAND) {
        const UINT command =
            static_cast<UINT>(
                wParam & 0xFFF0u);

        if (command == SC_MINIMIZE) {
            if (GetPropW(
                    hwnd,
                    kMinimizeBypass)) {

                RemovePropW(
                    hwnd,
                    kMinimizeBypass);

                return DefWindowProcW_Original(
                    hwnd,
                    message,
                    wParam,
                    lParam);
            }

            if (BeginMinimize(hwnd)) {
                return DefWindowProcW_Original(
                    hwnd,
                    message,
                    wParam,
                    lParam);
            }
        }

        if (command == SC_RESTORE &&
            IsIconic(hwnd) &&
            g_settings.restoreAnimation) {

            if (BeginRestore(
                    hwnd,
                    SW_RESTORE)) {
                return 0;
            }
        }

        if (command == SC_CLOSE) {
            if (GetPropW(
                    hwnd,
                    kCloseBypass)) {

                RemovePropW(
                    hwnd,
                    kCloseBypass);

                return DefWindowProcW_Original(
                    hwnd,
                    message,
                    wParam,
                    lParam);
            }

            if (BeginClose(
                    hwnd,
                    message,
                    wParam,
                    lParam)) {
                return 0;
            }
        }
    }

    if (message == WM_CLOSE) {
        if (GetPropW(
                hwnd,
                kCloseBypass)) {

            RemovePropW(
                hwnd,
                kCloseBypass);

            return DefWindowProcW_Original(
                hwnd,
                message,
                wParam,
                lParam);
        }

        if (BeginClose(
                hwnd,
                message,
                wParam,
                lParam)) {
            return 0;
        }
    }

    return DefWindowProcW_Original(
        hwnd,
        message,
        wParam,
        lParam);
}

BOOL WINAPI SetWindowPos_Hook(
    HWND hwnd,
    HWND insertAfter,
    int x,
    int y,
    int cx,
    int cy,
    UINT flags) {

    if ((flags & SWP_SHOWWINDOW) &&
        !IsWindowVisible(hwnd) &&
        !IsIconic(hwnd) &&
        g_settings.enabled &&
        g_settings.launchAnimation &&
        IsAnimatableWindow(hwnd) &&
        !IsExcluded(hwnd)) {

        if (MarkLaunchSeen(hwnd) &&
            MarkActive(hwnd)) {

            DisableNativeTransitions(
                hwnd);

            SetAlpha(hwnd, 0);

            BOOL result =
                SetWindowPos_Original(
                    hwnd,
                    insertAfter,
                    x,
                    y,
                    cx,
                    cy,
                    flags);

            Sleep(15);

            Frame* frame =
                CaptureWindow(hwnd);

            if (!frame) {
                SetAlpha(
                    hwnd,
                    255);
                EnableNativeTransitions(
                    hwnd);
                ClearActive(hwnd);
                return result;
            }

            const RECT windowRect =
                GetFrameRect(hwnd);

            const RECT iconRect =
                GetIconRect(
                    hwnd,
                    windowRect);

            SetPropW(
                hwnd,
                kHiddenByUs,
                reinterpret_cast<HANDLE>(1));

            Job* job =
                new Job();

            job->hwnd = hwnd;
            job->frame = frame;
            job->windowRect =
                windowRect;
            job->iconRect =
                iconRect;
            job->toDock =
                false;
            job->revealReal =
                true;

            if (!StartJob(job)) {
                RemovePropW(
                    hwnd,
                    kHiddenByUs);

                SetAlpha(
                    hwnd,
                    255);
                EnableNativeTransitions(
                    hwnd);
                ClearActive(hwnd);
            }

            return result;
        }
    }

    return SetWindowPos_Original(
        hwnd,
        insertAfter,
        x,
        y,
        cx,
        cy,
        flags);
}

BOOL Wh_ModInit() {
    LoadSettings();

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(ShowWindow),
            reinterpret_cast<void*>(ShowWindow_Hook),
            reinterpret_cast<void**>(
                &ShowWindow_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(ShowWindowAsync),
            reinterpret_cast<void*>(ShowWindowAsync_Hook),
            reinterpret_cast<void**>(
                &ShowWindowAsync_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(SetWindowPlacement),
            reinterpret_cast<void*>(SetWindowPlacement_Hook),
            reinterpret_cast<void**>(
                &SetWindowPlacement_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(CloseWindow),
            reinterpret_cast<void*>(CloseWindow_Hook),
            reinterpret_cast<void**>(
                &CloseWindow_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(DefWindowProcW),
            reinterpret_cast<void*>(DefWindowProcW_Hook),
            reinterpret_cast<void**>(
                &DefWindowProcW_Original))) {
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            reinterpret_cast<void*>(SetWindowPos),
            reinterpret_cast<void*>(SetWindowPos_Hook),
            reinterpret_cast<void**>(
                &SetWindowPos_Original))) {
        return FALSE;
    }

    Wh_Log(
        L"Win11 Genie Window Animation initialized");

    return TRUE;
}

void Wh_ModBeforeUninit() {
    g_unloading = true;

    for (int i = 0;
         i < 300;
         ++i) {

        bool empty = false;

        {
            std::lock_guard<std::mutex> lock(
                g_stateMutex);

            empty =
                g_active.empty();
        }

        if (empty) {
            break;
        }

        Sleep(10);
    }
}

void Wh_ModUninit() {
    g_unloading = true;

    std::lock_guard<std::mutex> lock(
        g_stateMutex);

    g_active.clear();
    g_launchSeen.clear();

    Wh_Log(
        L"Win11 Genie Window Animation uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

} // namespace
