/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "advanced_settings.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cst {
namespace {
enum Id {
    Width = 200,
    Height,
    Cell,
    Levels,
    Parity,
    Batch,
    SendFps,
    CaptureFps,
    AckMs,
    ControlMs,
    HoldMs,
    GapMs,
    KeyMode,
    FreshMs,
    Enabled,
    Click,
    Restore,
    Capacity,
    Defaults,
    Save,
    Cancel
};
struct Dialog {
    HWND window = nullptr;
    HFONT font = nullptr;
    Settings value;
    bool finished = false, saved = false;
    HWND item(int id) const { return GetDlgItem(window, id); }
    std::wstring text(int id) const {
        HWND h = item(id);
        std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
        GetWindowTextW(h, s.data(), int(s.size()));
        s.resize(wcslen(s.c_str()));
        return s;
    }
    unsigned number(int id, unsigned low, unsigned high) const {
        const auto s = text(id);
        size_t used = 0;
        unsigned long n = std::stoul(s, &used);
        if (used != s.size() || n < low || n > high)
            throw std::invalid_argument("Value outside range");
        return unsigned(n);
    }
    HWND add(const wchar_t *type, const wchar_t *text, DWORD style, int id, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
                                 WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }
    void field(const wchar_t *label, int id, int x, int y, bool combo = false) {
        add(L"STATIC", label, 0, 0, x, y + 4, 220, 24);
        add(combo ? L"COMBOBOX" : L"EDIT", L"",
            combo ? CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP : ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, id,
            x + 222, y, 132, combo ? 180 : 28);
    }
    void setNumber(int id, unsigned n) { SetWindowTextW(item(id), std::to_wstring(n).c_str()); }
    void combo(int id, const std::vector<unsigned> &values, unsigned selected) {
        SendMessageW(item(id), CB_RESETCONTENT, 0, 0);
        for (unsigned v : values) {
            auto text = std::to_wstring(v);
            LRESULT i = SendMessageW(item(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            SendMessageW(item(id), CB_SETITEMDATA, WPARAM(i), v);
            if (v == selected)
                SendMessageW(item(id), CB_SETCURSEL, WPARAM(i), 0);
        }
    }
    unsigned selected(int id) const {
        LRESULT at = SendMessageW(item(id), CB_GETCURSEL, 0, 0);
        if (at == CB_ERR)
            throw std::invalid_argument("Select an encoding option");
        return unsigned(SendMessageW(item(id), CB_GETITEMDATA, WPARAM(at), 0));
    }
    Settings read() const {
        Settings s = value;
        s.encoding.width = number(Width, 900, 1920);
        s.encoding.height = number(Height, 300, 1080);
        s.encoding.cell = selected(Cell);
        s.encoding.levels = selected(Levels);
        s.encoding.parity = selected(Parity);
        s.encoding.windowFrames = number(Batch, 16, 256);
        s.sendFps = number(SendFps, 1, 30);
        s.captureFps = number(CaptureFps, 1, 60);
        s.progressAckMs = number(AckMs, 500, 30000);
        s.controlAckMs = number(ControlMs, 250, 10000);
        s.keyHoldMs = number(HoldMs, 1, 200);
        s.keyGapMs = number(GapMs, 1, 200);
        s.imageFreshMs = number(FreshMs, 100, 5000);
        s.keyMode = FeedbackKeyMode(selected(KeyMode));
        s.feedbackEnabled = SendMessageW(item(Enabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
        s.clickCanvas = SendMessageW(item(Click), BM_GETCHECK, 0, 0) == BST_CHECKED;
        s.restoreMouse = SendMessageW(item(Restore), BM_GETCHECK, 0, 0) == BST_CHECKED;
        s.validate();
        return s;
    }
    void preview() {
        try {
            auto s = read();
            std::wostringstream text;
            text << L"每帧 " << s.encoding.slots() << L" 块 × " << s.encoding.payloadBytes()
                 << L" 字节\r\n理论载荷 "
                 << (uint64_t(s.encoding.slots()) * s.encoding.payloadBytes() * s.sendFps) / 1000.0
                 << L" KB/s（实际速度取决于链路）";
            SetWindowTextW(item(Capacity), text.str().c_str());
        } catch (const std::exception &) {
            SetWindowTextW(item(Capacity), L"请填写范围内的参数；画布须能容纳文件信息。 ");
        }
    }
    void fill() {
        setNumber(Width, value.encoding.width);
        setNumber(Height, value.encoding.height);
        combo(Cell, {2, 3, 4, 5, 6, 7, 8}, value.encoding.cell);
        combo(Levels, {2, 4, 8, 16}, value.encoding.levels);
        combo(Parity, {16, 24, 32, 48, 64}, value.encoding.parity);
        setNumber(Batch, value.encoding.windowFrames);
        setNumber(SendFps, value.sendFps);
        setNumber(CaptureFps, value.captureFps);
        setNumber(AckMs, value.progressAckMs);
        setNumber(ControlMs, value.controlAckMs);
        setNumber(HoldMs, value.keyHoldMs);
        setNumber(GapMs, value.keyGapMs);
        setNumber(FreshMs, value.imageFreshMs);
        SendMessageW(item(KeyMode), CB_RESETCONTENT, 0, 0);
        const wchar_t *names[] = {L"扫描码（推荐）", L"虚拟键", L"Unicode 字符"};
        for (unsigned i = 0; i < 3; ++i) {
            LRESULT n = SendMessageW(item(KeyMode), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i]));
            SendMessageW(item(KeyMode), CB_SETITEMDATA, WPARAM(n), i);
        }
        SendMessageW(item(KeyMode), CB_SETCURSEL, unsigned(value.keyMode), 0);
        for (auto pair : {std::pair<int, bool>{Enabled, value.feedbackEnabled},
                          {Click, value.clickCanvas},
                          {Restore, value.restoreMouse}})
            SendMessageW(item(pair.first), BM_SETCHECK, pair.second ? BST_CHECKED : BST_UNCHECKED, 0);
        preview();
    }
    void create() {
        add(L"STATIC", L"高级设置", 0, 0, 24, 16, 760, 28);
        add(L"STATIC", L"默认沿用已测通的参数；保存后用于下一次传输，接收端自动识别图像编码。", 0, 0, 24, 48,
            776, 28);
        add(L"STATIC", L"图像编码（发送端）", 0, 0, 24, 86, 354, 26);
        add(L"STATIC", L"刷新与回执", 0, 0, 420, 86, 354, 26);
        field(L"画布宽度（900–1920）", Width, 24, 118);
        field(L"画布高度（300–1080）", Height, 24, 152);
        field(L"格子大小（像素）", Cell, 24, 186, true);
        field(L"灰阶数量", Levels, 24, 220, true);
        field(L"纠错字节 ECC", Parity, 24, 254, true);
        field(L"每批帧数（16的倍数）", Batch, 24, 288);
        field(L"发送 FPS（1–30）", SendFps, 420, 118);
        field(L"抓取 FPS（1–60）", CaptureFps, 420, 152);
        field(L"进度回执 ms（500–30000）", AckMs, 420, 186);
        field(L"协商回执 ms（250–10000）", ControlMs, 420, 220);
        field(L"按键保持 ms（1–200）", HoldMs, 420, 254);
        field(L"按键间隔 ms（1–200）", GapMs, 420, 288);
        field(L"键盘方式", KeyMode, 420, 322, true);
        field(L"画面有效 ms（100–5000）", FreshMs, 420, 356);
        add(L"STATIC", L"", 0, Capacity, 24, 340, 354, 70);
        add(L"STATIC",
            L"画布须完整可见并保持1:"
            L"1显示。\r\n更高灰阶或更快刷新可能增加误码。\r\n更改编码后将使用独立的续传记录。",
            0, 0, 24, 426, 368, 92);
        add(L"BUTTON", L"启用自动回执", BS_AUTOCHECKBOX | WS_TABSTOP, Enabled, 420, 392, 354, 28);
        add(L"BUTTON", L"回执前自动点击云端画布", BS_AUTOCHECKBOX | WS_TABSTOP, Click, 420, 424, 354, 28);
        add(L"BUTTON", L"回执后还原鼠标（未手动移动时）", BS_AUTOCHECKBOX | WS_TABSTOP, Restore, 420, 456,
            378, 28);
        add(L"STATIC", L"关闭自动点击时请先点一次画布；停用自动回执可复制后在云端画布粘贴。", 0, 0, 420, 490,
            378, 40);
        add(L"BUTTON", L"恢复推荐参数", BS_PUSHBUTTON | WS_TABSTOP, Defaults, 24, 542, 168, 34);
        add(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, Save, 552, 542, 110, 34);
        add(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, Cancel, 680, 542, 110, 34);
        fill();
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto *self = reinterpret_cast<Dialog *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Dialog *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wp, lp);
        if (message == WM_CREATE) {
            self->create();
            return 0;
        }
        if (message == WM_COMMAND) {
            int id = LOWORD(wp);
            if (id == Save || id == IDOK) {
                try {
                    self->value = self->read();
                    self->saved = true;
                    self->finished = true;
                } catch (const std::exception &) {
                    MessageBoxW(window,
                                L"请检查各项范围、每批帧数须为16的倍数，以及画布容量。启用回执时，画面有效时"
                                L"间须覆盖一次抓取间隔并多留100ms。",
                                L"参数无效", MB_OK | MB_ICONINFORMATION);
                }
            } else if (id == Cancel || id == IDCANCEL)
                self->finished = true;
            else if (id == Defaults) {
                const auto original = self->value;
                self->value = Settings{};
                self->value.source = original.source;
                self->value.directory = original.directory;
                self->value.process = original.process;
                self->value.send = original.send;
                self->value.windowSaved = original.windowSaved;
                self->value.windowX = original.windowX;
                self->value.windowY = original.windowY;
                self->value.windowWidth = original.windowWidth;
                self->value.windowHeight = original.windowHeight;
                self->fill();
            } else if (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == BN_CLICKED)
                self->preview();
            return 0;
        }
        if (message == WM_CLOSE) {
            self->finished = true;
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
HWND create(Dialog &d, HWND owner) {
    WNDCLASSW cls{};
    cls.lpfnWndProc = Dialog::procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"CloudScreenTransferAdvanced";
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register advanced settings");
    RECT rect{0, 0, 820, 598};
    DWORD style = WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"CloudScreenTransfer · 高级设置", style,
                           work.left + std::max(0, (int(work.right - work.left) - w) / 2),
                           work.top + std::max(0, (int(work.bottom - work.top) - h) / 2), w, h, owner,
                           nullptr, cls.hInstance, &d);
}
} // namespace
bool editAdvancedSettings(HWND owner, HFONT font, Settings &settings) {
    Dialog d;
    d.font = font;
    d.value = settings;
    HWND window = create(d, owner);
    if (!window)
        throw std::runtime_error("Cannot create advanced settings");
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOWNORMAL);
    MSG message{};
    while (!d.finished) {
        int result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0)
                PostQuitMessage(int(message.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DestroyWindow(window);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (d.saved)
        settings = d.value;
    return d.saved;
}
bool advancedSettingsSmoke(HWND owner, HFONT font, const Settings &initial) {
    Dialog d;
    d.font = font;
    d.value = initial;
    HWND window = create(d, owner);
    if (!window)
        return false;
    const auto restored = d.read();
    bool restoredAll =
        restored.encoding == initial.encoding && restored.sendFps == initial.sendFps &&
        restored.captureFps == initial.captureFps && restored.progressAckMs == initial.progressAckMs &&
        restored.controlAckMs == initial.controlAckMs && restored.keyHoldMs == initial.keyHoldMs &&
        restored.keyGapMs == initial.keyGapMs && restored.imageFreshMs == initial.imageFreshMs &&
        restored.keyMode == initial.keyMode && restored.feedbackEnabled == initial.feedbackEnabled &&
        restored.clickCanvas == initial.clickCanvas && restored.restoreMouse == initial.restoreMouse;
    struct Bounds {
        HWND root;
        RECT client;
        bool fits = true;
    } bounds{window, {}};
    GetClientRect(window, &bounds.client);
    EnumChildWindows(
        window,
        [](HWND child, LPARAM parameter) -> BOOL {
            auto &b = *reinterpret_cast<Bounds *>(parameter);
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, b.root, reinterpret_cast<POINT *>(&rect), 2);
            if (rect.left < 0 || rect.top < 0 || rect.right > b.client.right || rect.bottom > b.client.bottom)
                b.fits = false;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&bounds));
    d.setNumber(SendFps, 8);
    d.combo(Parity, {16, 24, 32, 48, 64}, 64);
    d.combo(Levels, {2, 4, 8, 16}, 8);
    SendMessageW(d.item(Restore), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(window, WM_COMMAND, Save, 0);
    bool good = restoredAll && bounds.fits && d.saved && d.value.sendFps == 8 &&
                d.value.encoding.parity == 64 && d.value.encoding.levels == 8 && d.value.restoreMouse;
    d.saved = false;
    d.finished = false;
    SendMessageW(window, WM_COMMAND, Defaults, 0);
    good = good && d.read().encoding == Encoding{} && d.read().sendFps == 5;
    DestroyWindow(window);
    return good;
}
} // namespace cst
