/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "windows_channel.h"
#include "protocol.h"
#include <tlhelp32.h>
#include <imm.h>
#include <opencv2/imgproc.hpp>
#include <exception>
#include <set>
#include <stdexcept>

namespace cst {
namespace {
constexpr wchar_t CanvasClass[] = L"CloudScreenTransferOpticalCanvasV1";
std::runtime_error windowsError(const char *operation) {
    return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) +
                              ")");
}
struct WindowSearch {
    std::set<DWORD> processes;
    HWND result = nullptr;
    uint64_t area = 0;
};
BOOL CALLBACK chooseClient(HWND window, LPARAM parameter) {
    auto &search = *reinterpret_cast<WindowSearch *>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (!search.processes.count(process) || !IsWindowVisible(window) || IsIconic(window))
        return TRUE;
    RECT rect{};
    if (!GetClientRect(window, &rect) || rect.right < LONG(CanvasWidth) || rect.bottom < LONG(CanvasHeight))
        return TRUE;
    uint64_t area = uint64_t(rect.right) * uint64_t(rect.bottom);
    if (area > search.area) {
        search.area = area;
        search.result = window;
    }
    return TRUE;
}
bool modifiersHeld() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000)
            return true;
    return false;
}
bool inputMouse(DWORD flags) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    return SendInput(1, &input, sizeof(input)) == 1;
}
bool inputKey(WORD scan, bool release) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (release ? KEYEVENTF_KEYUP : 0);
    return SendInput(1, &input, sizeof(input)) == 1;
}
struct InputRelease {
    WORD scan = 0;
    bool mouse = false, active = true;
    bool release() noexcept {
        if (!active)
            return true;
        active = false;
        bool success = mouse ? inputMouse(MOUSEEVENTF_LEFTUP) : inputKey(scan, true);
        if (!success) {
            // Best-effort release if the desktop changes after the down event.
            if (mouse)
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            else
                keybd_event(BYTE(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK)), BYTE(scan), KEYEVENTF_KEYUP, 0);
        }
        return success;
    }
    ~InputRelease() { release(); }
};
} // namespace

HWND findClient(const std::wstring &executableName) {
    if (executableName.empty())
        return nullptr;
    WindowSearch search;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
        do {
            if (_wcsicmp(entry.szExeFile, executableName.c_str()) == 0)
                search.processes.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    EnumWindows(chooseClient, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

struct Capture::Impl {
    HDC screen = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    void *pixels = nullptr;
    int width = 0, height = 0;
    Impl() {
        screen = GetDC(nullptr);
        if (screen)
            memory = CreateCompatibleDC(screen);
        if (!screen || !memory) {
            if (screen)
                ReleaseDC(nullptr, screen);
            screen = nullptr;
            throw windowsError("Cannot create capture DC");
        }
    }
    void reset() {
        GdiFlush();
        if (original) {
            SelectObject(memory, original);
            original = nullptr;
        }
        if (bitmap) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
        pixels = nullptr;
        width = height = 0;
    }
    ~Impl() {
        reset();
        if (memory)
            DeleteDC(memory);
        if (screen)
            ReleaseDC(nullptr, screen);
    }
    cv::Mat take(HWND target) {
        if (!IsWindow(target) || !IsWindowVisible(target) || IsIconic(target))
            return {};
        RECT rect{};
        POINT origin{0, 0};
        if (!GetClientRect(target, &rect) || !ClientToScreen(target, &origin))
            return {};
        int w = rect.right, h = rect.bottom;
        if (w < int(CanvasWidth) || h < int(CanvasHeight) || w > 16384 || h > 16384)
            return {};
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (origin.x < x || origin.y < y || int64_t(origin.x) + w > int64_t(x) + vw ||
            int64_t(origin.y) + h > int64_t(y) + vh)
            return {};
        if (w != width || h != height) {
            reset();
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -h;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels) {
                reset();
                throw windowsError("Cannot create capture DIB");
            }
            auto selected = SelectObject(memory, bitmap);
            if (!selected || selected == HGDI_ERROR) {
                reset();
                throw windowsError("Cannot select capture DIB");
            }
            original = selected;
            width = w;
            height = h;
        }
        if (!BitBlt(memory, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT))
            return {};
        GdiFlush(); // Complete queued GDI writes before the CPU reads DIB pixels.
        return cv::Mat(h, w, CV_8UC4, pixels).clone();
    }
};
Capture::Capture() : impl(new Impl) {}
Capture::~Capture() = default;
cv::Mat Capture::take(HWND target) {
    return impl->take(target);
}

struct Canvas::Impl {
    HWND window = nullptr;
    DWORD thread = GetCurrentThreadId();
    bool alive = true, wrongSize = false;
    cv::Mat pixels;
    FeedbackParser parser;
    std::function<void(const Feedback &)> callback;
    std::exception_ptr callbackError;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = window;
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_SIZE && wparam != SIZE_MINIMIZED) {
            self->wrongSize = LOWORD(lparam) != CanvasWidth || HIWORD(lparam) != CanvasHeight;
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            if (!self->pixels.empty() && !self->wrongSize) {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = CanvasWidth;
                info.bmiHeader.biHeight = -LONG(CanvasHeight);
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                SetDIBitsToDevice(dc, 0, 0, CanvasWidth, CanvasHeight, 0, 0, 0, CanvasHeight,
                                  self->pixels.data, &info, DIB_RGB_COLORS);
                GdiFlush();
            } else
                FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_CHAR) {
            try {
                char character = wparam < 128 ? char(wparam) : 0;
                if (character >= 'a' && character <= 'z')
                    character -= char('a' - 'A');
                if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                    character == '\r') {
                    auto command = self->parser.push(character);
                    if (command && self->callback)
                        self->callback(*command);
                } else
                    self->parser.reset();
            } catch (...) {
                self->callbackError = std::current_exception();
            }
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            self->alive = false;
            return 0;
        }
        if (message == WM_NCDESTROY) {
            self->alive = false;
            self->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    explicit Impl(std::function<void(const Feedback &)> feedback) : callback(std::move(feedback)) {
        SetProcessDPIAware();
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = CanvasClass;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw windowsError("Cannot register optical canvas");
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT outer{0, 0, LONG(CanvasWidth), LONG(CanvasHeight)}, work{};
        if (!AdjustWindowRect(&outer, style, FALSE) || !SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
            throw windowsError("Cannot size optical canvas");
        int width = outer.right - outer.left, height = outer.bottom - outer.top;
        if (width > work.right - work.left || height > work.bottom - work.top)
            throw std::runtime_error("The 1000x600 canvas does not fit this desktop at native size");
        int x = work.left + (work.right - work.left - width) / 2,
            y = work.top + (work.bottom - work.top - height) / 2;
        window = CreateWindowExW(0, CanvasClass, L"CloudScreen Transfer - keep this window visible", style, x,
                                 y, width, height, nullptr, nullptr, cls.hInstance, this);
        if (!window)
            throw windowsError("Cannot create optical canvas");
        RECT client{};
        if (!GetClientRect(window, &client) || client.right != LONG(CanvasWidth) ||
            client.bottom != LONG(CanvasHeight)) {
            DestroyWindow(window);
            throw std::runtime_error("The optical canvas must remain exactly 1000x600 pixels");
        }
        ImmAssociateContext(window, nullptr); // Canvas only; leave other apps and system settings alone.
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
    }
    ~Impl() {
        if (window) {
            if (thread == GetCurrentThreadId())
                DestroyWindow(window);
            else {
                // Normal ownership is the creating worker. Do not leave a freed
                // Impl pointer attached if a caller accidentally destroys elsewhere.
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
        }
    }
    void requireThread() const {
        if (thread != GetCurrentThreadId())
            throw std::runtime_error("Canvas must be used on its creating worker thread");
    }
};
Canvas::Canvas(std::function<void(const Feedback &)> feedback) : impl(new Impl(std::move(feedback))) {}
Canvas::~Canvas() = default;
void Canvas::display(const cv::Mat &gray) {
    impl->requireThread();
    if (!impl->alive || !impl->window)
        return;
    if (impl->wrongSize)
        throw std::runtime_error("The optical canvas was resized; native pixels are required");
    if (gray.type() != CV_8UC1 || gray.cols != int(CanvasWidth) || gray.rows != int(CanvasHeight))
        throw std::invalid_argument("Canvas expects a native 1000x600 grayscale frame");
    cv::cvtColor(gray, impl->pixels, cv::COLOR_GRAY2BGRA); // Owned pixels survive the caller's next frame.
    RedrawWindow(impl->window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    GdiFlush();
}
bool Canvas::poll() {
    impl->requireThread();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            impl->alive = false;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (impl->callbackError)
        std::rethrow_exception(impl->callbackError);
    return impl->alive && impl->window;
}

bool sendFeedback(HWND target, const cv::Rect &canvas, const std::string &wire, Cancel interrupted,
                  std::wstring &failure) {
    failure.clear();
    Feedback parsed;
    try {
        if (wire.size() != FeedbackWireChars || wire.back() != '\r' || !parseFeedback(wire, parsed) ||
            formatFeedback(parsed) != wire) {
            failure = L"反馈格式或CRC无效";
            return false;
        }
    } catch (...) {
        failure = L"反馈格式无效";
        return false;
    }
    for (char character : wire)
        if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
              character == '\r')) {
            failure = L"反馈包含非协议按键";
            return false;
        }
    auto guard = [&]() -> bool {
        try {
            if (interrupted && interrupted()) {
                failure = L"反馈已取消或当前画面已过期";
                return false;
            }
        } catch (...) {
            failure = L"反馈时画面状态检查失败";
            return false;
        }
        if (!IsWindow(target) || !IsWindowVisible(target) || IsIconic(target) ||
            GetAncestor(GetForegroundWindow(), GA_ROOT) != target) {
            failure = L"请将云桌面窗口保持在前台";
            return false;
        }
        if (modifiersHeld()) {
            failure = L"请松开Ctrl、Shift、Alt和Windows键";
            return false;
        }
        RECT client{};
        if (canvas.width != int(CanvasWidth) || canvas.height != int(CanvasHeight) || canvas.x < 0 ||
            canvas.y < 0 || !GetClientRect(target, &client) ||
            int64_t(canvas.x) + canvas.width > client.right ||
            int64_t(canvas.y) + canvas.height > client.bottom) {
            failure = L"云端画布位置无效";
            return false;
        }
        POINT point{canvas.x + 500, canvas.y + 12};
        if (!ClientToScreen(target, &point)) {
            failure = L"无法定位云端画布";
            return false;
        }
        HWND hit = WindowFromPoint(point);
        if (hit != target && !IsChild(target, hit)) {
            failure = L"云端画布被其他窗口遮挡";
            return false;
        }
        return true;
    };
    if (!guard())
        return false;
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) &
        0x8000) {
        failure = L"请松开鼠标按键后重试反馈";
        return false;
    }
    POINT focus{canvas.x + 500, canvas.y + 12};
    if (!ClientToScreen(target, &focus) || !SetCursorPos(focus.x, focus.y)) {
        failure = L"无法点击云端画布";
        return false;
    }
    if (!guard())
        return false;
    POINT actual{};
    if (!GetCursorPos(&actual) || actual.x != focus.x || actual.y != focus.y) {
        failure = L"反馈点击前鼠标位置已改变";
        return false;
    }
    if (!inputMouse(MOUSEEVENTF_LEFTDOWN)) {
        inputMouse(MOUSEEVENTF_LEFTUP);
        failure = L"云桌面未接收鼠标反馈";
        return false;
    }
    InputRelease mouseRelease{0, true, true};
    Sleep(8);
    bool stillSafe = guard();
    bool released = mouseRelease.release(); // Release even if focus/cancel changed during the hold.
    if (!stillSafe)
        return false;
    if (!released) {
        failure = L"无法释放反馈鼠标按键";
        return false;
    }
    Sleep(12);
    for (char character : wire) {
        if (!guard())
            return false;
        WORD key = character == '\r' ? VK_RETURN : WORD(uint8_t(character));
        WORD scan = WORD(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
        if (!scan) {
            failure = L"无法转换协议按键扫描码";
            return false;
        }
        if (GetAsyncKeyState(key) & 0x8000) {
            failure = L"请松开正在按住的协议字符键";
            return false;
        }
        if (!inputKey(scan, false)) {
            inputKey(scan, true);
            failure = L"云桌面未接收键盘反馈";
            return false;
        }
        InputRelease keyRelease{scan, false, true};
        Sleep(8);
        stillSafe = guard();
        released = keyRelease.release(); // Never leave an injected key held on any interrupted path.
        if (!stillSafe)
            return false;
        if (!released) {
            failure = L"无法释放反馈键盘按键";
            return false;
        }
        Sleep(12);
    }
    return true;
}
} // namespace cst
