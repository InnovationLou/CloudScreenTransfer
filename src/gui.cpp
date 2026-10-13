/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include "file_transfer.h"
#include "frame_codec.h"
#include "windows_channel.h"
#include "protocol.h"
#include "image_runtime.h"
#include "diagnostics.h"
#include "settings.h"
#include "advanced_settings.h"
#include <algorithm>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <memory>
#include <stdexcept>
#include <typeinfo>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT UpdateMessage = WM_APP + 1, FinishMessage = WM_APP + 2;
enum Control {
    SendRole = 100,
    ReceiveRole,
    PathEdit,
    PathLabel,
    BrowseButton,
    ProcessEdit,
    StartButton,
    StopButton,
    FolderButton,
    ProgressBar,
    StatusLabel,
    DetailLabel,
    LogEdit,
    AdvancedButton,
    CopyFeedbackButton,
    EncodingLabel
};
struct Ticket {
    HWND target = nullptr;
    cv::Rect canvas;
    cst::Feedback feedback;
    Clock::time_point seen{};
    bool valid = false;
};
struct Update {
    cst::Progress progress;
    std::wstring note;
    bool hasProgress = true, error = false, status = true;
};
struct Application {
    HWND window = nullptr, log = nullptr;
    HFONT font = nullptr, heading = nullptr;
    std::atomic<bool> stopped{false};
    std::thread worker;
    std::mutex ticketMutex;
    Ticket ticket;
    bool running = false, closing = false, smoke = false, senderSmoke = false, autoReceive = false,
         lastRoleSend = false;
    bool smokeFault = false;
    bool initializing = true, settingsSmoke = false, processOverride = false;
    cst::Settings settings;
    std::wstring settingsPath, settingsWarning;
    std::atomic<unsigned> smokeFrames{0};
    int smokeResult = 1;
    std::wstring source, directory, process = L"uSmartView_VDI_Client.exe", lastMessage, completedPath,
                                    lastError;
    Clock::time_point started{};
} app;
HWND item(int id) {
    return GetDlgItem(app.window, id);
}
std::wstring textOf(HWND h) {
    std::wstring value(GetWindowTextLengthW(h) + 1, L'\0');
    GetWindowTextW(h, value.data(), int(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}
bool sender() {
    return SendMessageW(item(SendRole), BM_GETCHECK, 0, 0) == BST_CHECKED;
}
std::wstring widen(const std::string &text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}
std::wstring describeFailure(std::exception_ptr failure, bool &wasCancelled) {
    wasCancelled = false;
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception &error) {
        wasCancelled = std::string(error.what()) == "Transfer cancelled";
        cst::diagnostics::exception(typeid(error).name(), error.what());
        return widen(error.what());
    } catch (...) {
        cst::diagnostics::exception("non-standard C++ exception", "no std::exception message");
        return L"非标准C++异常；详情已记录到CloudScreenTransfer.log";
    }
}
std::wstring bytes(uint64_t count) {
    const wchar_t *units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
    long double value = static_cast<long double>(count);
    unsigned unit = 0;
    while (value >= 1024 && unit < 5) {
        value /= 1024;
        ++unit;
    }
    std::wostringstream out;
    out << std::fixed << std::setprecision(unit ? 2 : 0) << value << L' ' << units[unit];
    return out.str();
}
void log(const std::wstring &value) {
    int n = GetWindowTextLengthW(app.log);
    if (n > 100000) {
        SendMessageW(app.log, EM_SETSEL, 0, 25000);
        SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(app.log, EM_SETSEL, WPARAM(-1), LPARAM(-1));
    SendMessageW(app.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((value + L"\r\n").c_str()));
    SendMessageW(app.log, EM_SCROLLCARET, 0, 0);
}
void post(const cst::Progress &progress, std::wstring note = {}) {
    auto value = std::make_unique<Update>();
    value->progress = progress;
    value->note = std::move(note);
    if (PostMessageW(app.window, UpdateMessage, 0, reinterpret_cast<LPARAM>(value.get())))
        value.release();
}
void postNote(std::wstring note, bool error = false, bool status = true) {
    auto value = std::make_unique<Update>();
    value->note = std::move(note);
    value->hasProgress = false;
    value->error = error;
    value->status = status;
    if (PostMessageW(app.window, UpdateMessage, 0, reinterpret_cast<LPARAM>(value.get())))
        value.release();
}
void enabled() {
    for (int id : {SendRole, ReceiveRole, PathEdit, BrowseButton, StartButton, ProcessEdit, AdvancedButton})
        EnableWindow(item(id), !app.running);
    EnableWindow(item(ProcessEdit), !app.running && !sender());
    EnableWindow(item(StopButton), app.running);
    EnableWindow(item(CopyFeedbackButton), app.running && !sender());
}
void refreshEncodingLabel() {
    const auto &s = app.settings;
    const auto &e = s.encoding;
    std::wostringstream label;
    label << e.width << L"×" << e.height << L" · " << e.cell << L"px / " << e.levels << L"灰 · " << s.sendFps
          << L" FPS · ECC" << e.parity << L"　｜　接收 " << s.captureFps << L" FPS";
    SetWindowTextW(item(EncodingLabel), label.str().c_str());
}
void captureMainOptions() {
    bool send = sender();
    if (send)
        app.source = textOf(item(PathEdit));
    else
        app.directory = textOf(item(PathEdit));
    app.process = textOf(item(ProcessEdit));
    app.settings.send = send;
    app.settings.source = app.source;
    app.settings.directory = app.directory;
    app.settings.process = app.process;
    WINDOWPLACEMENT position{sizeof(position)};
    if (GetWindowPlacement(app.window, &position)) {
        const auto &r = position.rcNormalPosition;
        app.settings.windowSaved = true;
        app.settings.windowX = r.left;
        app.settings.windowY = r.top;
        app.settings.windowWidth = std::max(900, int(r.right - r.left));
        app.settings.windowHeight = std::max(650, int(r.bottom - r.top));
    }
}
bool persistOptions() {
    if (app.smoke || app.initializing)
        return false;
    try {
        captureMainOptions();
        cst::saveSettings(app.settingsPath, app.settings);
        return true;
    } catch (...) {
        bool ignored = false;
        log(L"设置保存失败：" + describeFailure(std::current_exception(), ignored));
    }
    return false;
}
void role() {
    if (app.running)
        return;
    bool send = sender();
    if (send != app.lastRoleSend) {
        if (app.lastRoleSend)
            app.source = textOf(item(PathEdit));
        else
            app.directory = textOf(item(PathEdit));
        app.lastRoleSend = send;
    }
    SetWindowTextW(item(PathLabel), send ? L"发送文件" : L"保存目录");
    SetWindowTextW(item(PathEdit), (send ? app.source : app.directory).c_str());
    SetWindowTextW(item(BrowseButton), send ? L"选择文件…" : L"选择目录…");
    enabled();
    if (!app.initializing)
        SetTimer(app.window, 1, 500, nullptr);
}
std::wstring quote(const std::wstring &argument) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        if (c == L'\"')
            out.push_back(L'\\');
        out.push_back(c);
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}
bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("Cannot read process integrity");
    TOKEN_ELEVATION level{};
    DWORD n = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &level, sizeof(level), &n);
    CloseHandle(token);
    if (!ok)
        throw std::runtime_error("Cannot read process elevation");
    return level.TokenIsElevated != 0;
}
bool elevateReceiver(const std::wstring &directory, const std::wstring &process) {
    if (elevated())
        return false;
    std::wstring executable(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (!n || n >= executable.size())
        throw std::runtime_error("Cannot locate executable");
    executable.resize(n);
    auto parameters = L"--receive " + quote(directory) + L" --process " + quote(process) + L" --config " +
                      quote(app.settingsPath);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
        throw std::runtime_error(
            "Administrator launch was cancelled or failed; keyboard feedback requires matching integrity");
    return true;
}
void browse() {
    if (sender()) {
        std::vector<wchar_t> path(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = app.window;
        ofn.lpstrFilter = L"所有文件\0*.*\0\0";
        ofn.lpstrFile = path.data();
        ofn.nMaxFile = DWORD(path.size());
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            app.source = path.data();
            SetWindowTextW(item(PathEdit), app.source.c_str());
        }
    } else {
        BROWSEINFOW info{};
        info.hwndOwner = app.window;
        info.lpszTitle = L"选择接收文件保存目录";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&info);
        if (pid) {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(pid, path)) {
                app.directory = path;
                SetWindowTextW(item(PathEdit), app.directory.c_str());
            }
            CoTaskMemFree(pid);
        }
    }
}
void copyFeedback() {
    Ticket ticket;
    {
        std::lock_guard<std::mutex> lock(app.ticketMutex);
        ticket = app.ticket;
    }
    if (!ticket.valid || !ticket.feedback.transfer) {
        log(L"尚未识别有效图像，请稍后再复制回执。");
        return;
    }
    auto wire = widen(cst::formatFeedback(ticket.feedback));
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wire.size() + 1) * sizeof(wchar_t));
    if (!memory)
        throw std::runtime_error("Cannot allocate feedback clipboard");
    auto *data = static_cast<wchar_t *>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        throw std::runtime_error("Cannot lock feedback clipboard");
    }
    std::copy(wire.begin(), wire.end(), data);
    data[wire.size()] = L'\0';
    GlobalUnlock(memory);
    if (!OpenClipboard(app.window)) {
        GlobalFree(memory);
        throw std::runtime_error("Cannot open clipboard");
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        throw std::runtime_error("Cannot copy feedback");
    }
    CloseClipboard();
    log(L"回执已复制。在云端数据画布中按 Ctrl+V 可手动返回。");
}
bool ticketFresh(const Ticket &ticket) {
    std::lock_guard<std::mutex> lock(app.ticketMutex);
    return !app.stopped && app.ticket.valid && app.ticket.target == ticket.target &&
           app.ticket.canvas == ticket.canvas && app.ticket.feedback.transfer == ticket.feedback.transfer &&
           Clock::now() - app.ticket.seen < std::chrono::milliseconds(app.settings.imageFreshMs);
}
cst::Feedback feedbackForFrame(cst::Feedback command, const cst::FrameInfo &frame) {
    if (command.operation == 'M' && !command.transfer)
        command.transfer = frame.transfer;
    return command;
}
void feedbackLoop() {
    cst::diagnostics::stage("keyboard_feedback");
    Clock::time_point last{};
    std::wstring previousFailure;
    cst::Feedback lastCommand;
    while (!app.stopped) {
        Ticket current;
        {
            std::lock_guard<std::mutex> lock(app.ticketMutex);
            current = app.ticket;
        }
        if (app.settings.feedbackEnabled && current.valid && current.feedback.transfer &&
            Clock::now() - current.seen < std::chrono::milliseconds(app.settings.imageFreshMs)) {
            auto wire = cst::formatFeedback(current.feedback);
            auto interval = std::chrono::milliseconds(
                current.feedback.operation == 'P' ? app.settings.progressAckMs : app.settings.controlAckMs);
            bool phaseChanged = current.feedback.operation != lastCommand.operation ||
                                current.feedback.transfer != lastCommand.transfer ||
                                current.feedback.window != lastCommand.window;
            if (phaseChanged || Clock::now() - last >= interval) {
                std::wstring failure;
                bool sent = cst::sendFeedback(
                    current.target, current.canvas, wire, [&] { return !ticketFresh(current); }, failure,
                    app.settings);
                last = Clock::now();
                lastCommand = current.feedback;
                if (sent) {
                    if (!previousFailure.empty())
                        postNote(L"回执已恢复。", false, false);
                    previousFailure.clear();
                } else if (!app.stopped && failure != previousFailure) {
                    postNote(L"回执暂缓（画面接收继续）：" + failure, false, false);
                    previousFailure = failure;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void work(bool send, std::wstring path, std::wstring process) {
    bool success = false, failed = false;
    std::atomic<bool> feedbackFailed{false};
    std::thread feedback;
    auto last = Clock::time_point{};
    bool lastHash = false;
    uint64_t freshBytes = 0;
    std::unique_ptr<cst::Receiver> receiverForSave;
    try {
        auto progress = [&](const cst::Progress &p) {
            if (p.complete)
                success = true;
            freshBytes +=
                p.newBytes; // Engine notifications carry a delta, even when UI updates are throttled.
            auto now = Clock::now();
            if (now - last >= std::chrono::milliseconds(200) || p.complete || p.hashing != lastHash) {
                auto displayed = p;
                displayed.newBytes = freshBytes;
                post(displayed);
                last = now;
                lastHash = p.hashing;
            }
        };
        cst::diagnostics::stage("prepare_image_codec");
        cst::FrameCodec codec;
        if (send) {
            cst::diagnostics::stage("open_and_hash_source");
            cst::Sender source(path, progress, [] { return app.stopped.load(); }, app.settings.encoding);
            cst::diagnostics::stage("create_sender_canvas");
            cst::Canvas canvas([&](const cst::Feedback &f) { source.feedback(f); }, !app.smoke,
                               app.settings.encoding);
            uint32_t sequence = 0;
            auto next = Clock::now();
            Clock::time_point done{};
            while (!app.stopped && canvas.poll()) {
                auto now = Clock::now();
                if (now >= next) {
                    cst::diagnostics::stage("encode_frame");
                    auto frame = codec.encode(source.next(sequence++));
                    cst::diagnostics::stage("render_frame");
                    if (app.senderSmoke && app.smokeFault && app.smokeFrames == 1)
                        throw 7; // Exercise a non-std exception after a canvas has already rendered.
                    canvas.display(frame);
                    if (app.senderSmoke && ++app.smokeFrames >= 3) {
                        app.stopped = true;
                        break;
                    }
                    progress(source.progress());
                    next = now + std::chrono::nanoseconds(1000000000 / app.settings.sendFps);
                    if (source.complete()) {
                        if (done == Clock::time_point{})
                            done = now;
                        if (now - done >= std::chrono::seconds(5)) {
                            success = true;
                            break;
                        }
                    }
                }
                cst::diagnostics::stage("sender_wait");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (source.complete())
                success = true;
        } else {
            cst::diagnostics::stage("prepare_receiver");
            receiverForSave =
                std::make_unique<cst::Receiver>(path, progress, [] { return app.stopped.load(); });
            auto &receiver = *receiverForSave;
            cst::Capture capture;
            feedback = std::thread([&] {
                try {
                    feedbackLoop();
                } catch (...) {
                    feedbackFailed = true;
                    app.stopped = true;
                    bool cancelled = false;
                    postNote(L"回执失败：" + describeFailure(std::current_exception(), cancelled), true);
                }
            });
            HWND client = nullptr;
            auto report = Clock::now();
            Clock::time_point done{};
            while (!app.stopped) {
                auto tick = Clock::now();
                if (!client || !IsWindow(client) || !IsWindowVisible(client) || IsIconic(client))
                    client = cst::findClient(process);
                cst::diagnostics::stage("capture_screen");
                auto image = capture.take(client);
                cst::diagnostics::stage("decode_frame");
                auto decoded = image.empty() ? cst::DecodeResult{} : codec.decode(image);
                auto observed = Clock::now();
                if (decoded.valid) {
                    cst::diagnostics::stage("receive_file_data");
                    receiver.ingest(decoded.frame);
                    auto command = feedbackForFrame(receiver.feedback(), decoded.frame.info);
                    // A restarting receiver can request metadata from a CRC-verified data header.
                    {
                        std::lock_guard<std::mutex> lock(app.ticketMutex);
                        app.ticket = {client, decoded.canvas, command, observed,
                                      command.transfer == decoded.frame.info.transfer &&
                                          command.transfer != 0};
                    }
                    progress(receiver.progress());
                    if (receiver.complete()) {
                        if (done == Clock::time_point{})
                            done = Clock::now();
                        if (decoded.frame.info.complete || Clock::now() - done >= std::chrono::seconds(12)) {
                            success = true;
                            break;
                        }
                    }
                } else {
                    {
                        std::lock_guard<std::mutex> lock(app.ticketMutex);
                        app.ticket.valid = false;
                    }
                    if (Clock::now() - report >= std::chrono::seconds(2)) {
                        postNote(client ? L"等待完整图像，请保持1:1显示并切到云桌面窗口。"
                                        : L"等待云桌面客户端：" + process);
                        report = Clock::now();
                    }
                }
                // Completion is durable even if the canvas disappears during the final acknowledgement grace
                // period.
                if (receiver.complete() && done != Clock::time_point{} &&
                    Clock::now() - done >= std::chrono::seconds(12)) {
                    success = true;
                    break;
                }
                std::this_thread::sleep_until(tick +
                                              std::chrono::nanoseconds(1000000000 / app.settings.captureFps));
            }
            cst::diagnostics::stage("save_progress");
            receiver.save();
            if (receiver.complete())
                success = true;
        }
    } catch (...) {
        const char *failedStage = cst::diagnostics::stage();
        bool wasCancelled = false;
        auto error = describeFailure(std::current_exception(), wasCancelled);
        // Keep the receiver alive through cancellation so saving errors reach the UI.
        std::wstring saveFailure;
        try {
            if (receiverForSave) {
                cst::diagnostics::stage("save_progress_after_failure");
                receiverForSave->save();
            }
        } catch (...) {
            failed = true;
            bool ignored = false;
            saveFailure = L"；保存进度失败：" + describeFailure(std::current_exception(), ignored);
        }
        if (!app.stopped || !wasCancelled) {
            failed = true;
            postNote(L"任务失败（" + widen(failedStage) + L"）：" + error + saveFailure, true);
        } else if (!saveFailure.empty())
            postNote(L"停止时" + saveFailure, true);
    }
    cst::diagnostics::stage("worker_cleanup");
    app.stopped = true;
    if (feedback.joinable())
        feedback.join();
    failed = failed || feedbackFailed;
    receiverForSave.reset();
    {
        std::lock_guard<std::mutex> lock(app.ticketMutex);
        app.ticket.valid = false;
    }
    PostMessageW(app.window, FinishMessage, failed ? 2 : success ? 0 : 1, 0);
    cst::diagnostics::append(failed    ? "worker_finished result=failed\r\n"
                             : success ? "worker_finished result=complete\r\n"
                                       : "worker_finished result=stopped\r\n");
}
void start() {
    if (app.running)
        return;
    // A stopped/failed job must be joined before assigning another std::thread.
    if (app.worker.joinable())
        app.worker.join();
    persistOptions();
    bool send = sender();
    auto path = textOf(item(PathEdit)), process = textOf(item(ProcessEdit));
    if (path.empty() || (!send && process.empty())) {
        MessageBoxW(app.window, L"请选择文件或保存目录。", L"需要路径", MB_OK | MB_ICONINFORMATION);
        return;
    }
    try {
        if (!send && elevateReceiver(path, process)) {
            DestroyWindow(app.window);
            return;
        }
    } catch (const std::exception &error) {
        MessageBoxW(app.window, widen(error.what()).c_str(), L"接收权限", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (send)
        app.source = path;
    else
        app.directory = path;
    app.process = process;
    app.stopped = false;
    app.running = true;
    app.lastMessage.clear();
    app.lastError.clear();
    app.completedPath.clear();
    app.smokeFrames = 0;
    app.started = Clock::now();
    enabled();
    SendMessageW(item(ProgressBar), PBM_SETPOS, 0, 0);
    SetWindowTextW(item(DetailLabel), L"准备文件传输…");
    SetWindowTextW(item(StatusLabel), send ? L"正在检查源文件…" : L"等待发送端图像…");
    log(send ? L"发送开始：" + path : L"接收开始：" + path);
    cst::diagnostics::stage("launch_worker");
    cst::diagnostics::append(send ? "sender_start\r\n" : "receiver_start\r\n");
    try {
        app.worker = std::thread(work, send, std::move(path), std::move(process));
    } catch (...) {
        app.running = false;
        app.stopped = true;
        enabled();
        bool ignored = false;
        app.lastError = L"无法开始任务：" + describeFailure(std::current_exception(), ignored);
        SetWindowTextW(item(StatusLabel), app.lastError.c_str());
        log(app.lastError);
    }
}
HWND control(const wchar_t *klass, const wchar_t *text, DWORD style, int id, int x, int y, int w, int h) {
    HWND window = CreateWindowExW(std::wstring(klass) == L"EDIT" ? WS_EX_CLIENTEDGE : 0, klass, text,
                                  WS_CHILD | WS_VISIBLE | style, x, y, w, h, app.window,
                                  reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return window;
}
void controls() {
    app.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    app.heading =
        CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HWND title = control(L"STATIC", L"CloudScreenTransfer 图像文件传输", 0, 0, 24, 20, 810, 36);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app.heading), TRUE);
    control(L"STATIC", L"", 0, EncodingLabel, 24, 64, 800, 26);
    control(L"BUTTON", L"云桌面发送", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, SendRole, 24, 108, 190, 30);
    control(L"BUTTON", L"物理机接收", BS_AUTORADIOBUTTON | WS_TABSTOP, ReceiveRole, 240, 108, 190, 30);
    SendMessageW(item(ReceiveRole), BM_SETCHECK, app.settings.send ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(item(SendRole), BM_SETCHECK, app.settings.send ? BST_CHECKED : BST_UNCHECKED, 0);
    app.lastRoleSend = app.settings.send;
    control(L"STATIC", L"保存目录", 0, PathLabel, 24, 160, 90, 26);
    control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, PathEdit, 122, 153, 560, 32);
    control(L"BUTTON", L"选择目录…", BS_PUSHBUTTON | WS_TABSTOP, BrowseButton, 696, 152, 150, 34);
    control(L"STATIC", L"客户端进程", 0, 0, 24, 210, 94, 26);
    control(L"EDIT", app.process.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ProcessEdit, 122, 203, 560, 32);
    control(L"STATIC", L"发送端无需设置", 0, 0, 696, 210, 150, 26);
    control(L"BUTTON", L"开始", BS_DEFPUSHBUTTON | WS_TABSTOP, StartButton, 24, 254, 135, 38);
    control(L"BUTTON", L"停止并保存进度", BS_PUSHBUTTON | WS_TABSTOP, StopButton, 179, 254, 180, 38);
    control(L"BUTTON", L"打开保存目录", BS_PUSHBUTTON | WS_TABSTOP, FolderButton, 379, 254, 155, 38);
    control(L"BUTTON", L"高级设置…", BS_PUSHBUTTON | WS_TABSTOP, AdvancedButton, 554, 254, 126, 38);
    control(L"BUTTON", L"复制回执", BS_PUSHBUTTON | WS_TABSTOP, CopyFeedbackButton, 700, 254, 146, 38);
    control(L"STATIC", L"先开始物理机接收，再选择云端文件发送。", 0, StatusLabel, 24, 322, 820, 30);
    control(PROGRESS_CLASSW, L"", 0, ProgressBar, 24, 367, 822, 22);
    SendMessageW(item(ProgressBar), PBM_SETRANGE32, 0, 1000);
    control(L"STATIC", L"支持空文件、极小文件和64位大文件。", 0, DetailLabel, 24, 407, 820, 28);
    control(L"STATIC", L"接收时让云桌面保持前台，图像完整可见且1:1显示；回执会通过键盘发送。", 0, 0, 24, 452,
            820, 28);
    app.log = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                      LogEdit, 24, 492, 822, 160);
    SendMessageW(app.log, EM_SETLIMITTEXT, 300000, 0);
    role();
    refreshEncodingLabel();
    log(L"文件先写入临时文件；仅SHA-"
        L"256通过后显示完成并生成最终文件。接收开始时自动请求管理员权限。离开云桌面前台会暂停回执。 ");
}
std::wstring statusText(const cst::Progress &p) {
    if (p.hashing)
        return L"正在计算SHA-256…";
    if (p.complete)
        return L"文件完成，SHA-256校验通过";
    if (p.message == L"Waiting for file metadata")
        return L"等待发送端文件信息…";
    if (p.message == L"Waiting for receiver metadata acknowledgement")
        return L"等待接收端确认文件信息…";
    if (p.message == L"Resending metadata")
        return L"正在重新发送文件信息…";
    if (p.message == L"Sending requested frames")
        return L"正在发送缺少的数据…";
    if (p.message == L"Receiving file")
        return L"正在接收文件…";
    if (p.message == L"Resuming received file" || p.message == L"Resuming validated checkpoint")
        return L"已恢复进度，继续传输…";
    if (p.message == L"Window saved; requesting next window")
        return L"进度已保存，等待下一批数据…";
    if (p.message == L"Invalid metadata; requesting another announcement")
        return L"文件信息校验失败，正在重试…";
    return p.message.empty() ? L"正在传输…" : p.message;
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        app.window = window;
        controls();
        return 0;
    case WM_SIZE:
        if (app.log)
            MoveWindow(app.log, 24, 492, std::max(100, int(LOWORD(lp)) - 48),
                       std::max(80, int(HIWORD(lp)) - 516), TRUE);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(lp)->ptMinTrackSize = {900, 650};
        return 0;
    case WM_MOVE:
        if (!app.initializing)
            SetTimer(window, 1, 500, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(window, 1);
            persistOptions();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == StartButton)
            start();
        else if (LOWORD(wp) == StopButton) {
            app.stopped = true;
            EnableWindow(item(StopButton), FALSE);
            SetWindowTextW(item(StatusLabel), L"正在保存进度，请稍候…");
        } else if (LOWORD(wp) == BrowseButton)
            browse();
        else if (LOWORD(wp) == FolderButton) {
            auto directory = app.directory;
            if (directory.empty())
                return 0;
            if (reinterpret_cast<INT_PTR>(
                    ShellExecuteW(window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                log(L"保存目录尚未创建或无法打开。");
        } else if (LOWORD(wp) == AdvancedButton && !app.running) {
            try {
                captureMainOptions();
                if (cst::editAdvancedSettings(window, app.font, app.settings)) {
                    refreshEncodingLabel();
                    if (persistOptions())
                        log(L"高级设置已保存，下次启动会恢复这些参数。");
                }
            } catch (...) {
                bool ignored = false;
                log(L"设置失败：" + describeFailure(std::current_exception(), ignored));
            }
        } else if (LOWORD(wp) == CopyFeedbackButton) {
            try {
                copyFeedback();
            } catch (...) {
                bool ignored = false;
                log(L"复制失败：" + describeFailure(std::current_exception(), ignored));
            }
        } else if ((LOWORD(wp) == SendRole || LOWORD(wp) == ReceiveRole) && HIWORD(wp) == BN_CLICKED)
            role();
        else if ((LOWORD(wp) == PathEdit || LOWORD(wp) == ProcessEdit) && HIWORD(wp) == EN_CHANGE &&
                 !app.initializing)
            SetTimer(window, 1, 500, nullptr);
        return 0;
    case UpdateMessage: {
        std::unique_ptr<Update> u(reinterpret_cast<Update *>(lp));
        const auto &p = u->progress;
        if (u->hasProgress) {
            long double percent = p.total
                                      ? std::min<long double>(1, static_cast<long double>(p.done) / p.total)
                                  : p.complete ? 1
                                               : 0;
            SendMessageW(item(ProgressBar), PBM_SETPOS, WPARAM(percent * 1000), 0);
            SetWindowTextW(item(StatusLabel), statusText(p).c_str());
            double elapsed = std::chrono::duration<double>(Clock::now() - app.started).count();
            std::wostringstream detail;
            detail << bytes(p.done) << L" / " << bytes(p.total) << L"　" << std::fixed << std::setprecision(1)
                   << percent * 100 << L"%";
            if (!sender() && !p.hashing && elapsed > 0)
                detail << L"　本次平均 " << p.newBytes / elapsed / 1000 << L" KB/s";
            if (p.resumed)
                detail << L"　已恢复进度";
            SetWindowTextW(item(DetailLabel), detail.str().c_str());
        }
        if (!u->note.empty()) {
            if (u->error)
                app.lastError = u->note;
            if (u->status)
                SetWindowTextW(item(StatusLabel), u->note.c_str());
            if (u->note != app.lastMessage) {
                log(u->note);
                app.lastMessage = u->note;
            }
        } else if (u->hasProgress) {
            auto status = statusText(p);
            if (status != app.lastMessage) {
                log(status);
                app.lastMessage = status;
            }
        }
        if (p.complete && !p.path.empty() && p.path != app.completedPath) {
            log(L"已校验文件：" + p.path);
            app.completedPath = p.path;
        }
        return 0;
    }
    case FinishMessage:
        if (app.worker.joinable())
            app.worker.join();
        app.running = false;
        enabled();
        if (wp == 0) {
            SetWindowTextW(item(StatusLabel), L"文件完成，SHA-256校验通过");
            log(L"本次文件已完成并校验。");
        } else if (wp == 1) {
            SetWindowTextW(item(StatusLabel), L"已停止，已保存进度可用于续传。");
            log(L"任务已停止；未完成的文件保留在临时文件中。");
        } else {
            SetWindowTextW(item(StatusLabel),
                           app.lastError.empty() ? L"任务失败，请查看下方日志。" : app.lastError.c_str());
            log(L"任务失败，详情见上方错误；未通过校验的文件不会交付。");
        }
        if (app.closing)
            DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        persistOptions();
        if (app.running) {
            app.closing = true;
            app.stopped = true;
            EnableWindow(item(StopButton), FALSE);
            SetWindowTextW(item(StatusLabel), L"正在保存进度，请稍候…");
        } else
            DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
bool smoke() {
    bool valid = !sender() && !app.running && IsWindowEnabled(item(StartButton)) &&
                 !IsWindowEnabled(item(StopButton)) && IsWindowEnabled(item(ProcessEdit));
    auto sample = cst::Feedback{'P', 123, 0, {}};
    valid = valid && cst::parseFeedback(cst::formatFeedback(sample), sample);
    auto requested = feedbackForFrame(cst::Feedback{}, cst::FrameInfo{123, 0, 1, false});
    valid = valid && requested.operation == 'M' && requested.transfer == 123;
    app.started = Clock::now() - std::chrono::seconds(1);
    auto update = new Update;
    update->progress.done = 400;
    update->progress.total = 1000;
    update->progress.newBytes = 400;
    SendMessageW(app.window, UpdateMessage, 0, reinterpret_cast<LPARAM>(update));
    auto detail = textOf(item(DetailLabel));
    auto beforeNote = textOf(item(StatusLabel));
    auto note = new Update;
    note->hasProgress = false;
    note->status = false;
    note->note = L"回执等待";
    SendMessageW(app.window, UpdateMessage, 0, reinterpret_cast<LPARAM>(note));
    valid = valid && SendMessageW(item(ProgressBar), PBM_GETPOS, 0, 0) == 400 &&
            textOf(item(DetailLabel)) == detail && textOf(item(StatusLabel)) == beforeNote;
    SendMessageW(item(ReceiveRole), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(item(SendRole), BM_SETCHECK, BST_CHECKED, 0);
    role();
    return valid && sender() && !IsWindowEnabled(item(ProcessEdit)) &&
           IsWindowEnabled(item(AdvancedButton)) && cst::advancedSettingsSmoke(app.window, app.font);
}
bool senderSmoke() {
    // Run the actual GUI Start -> worker -> image -> Finish lifecycle. The
    // regular controls smoke test never started a sender or displayed a frame.
    wchar_t temporary[MAX_PATH]{}, source[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, temporary);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(temporary, L"cst", 0, source))
        throw std::runtime_error("Cannot create sender smoke fixture");
    struct Cleanup {
        const wchar_t *path;
        ~Cleanup() {
            app.stopped = true;
            if (app.worker.joinable())
                app.worker.join();
            app.running = false;
            DeleteFileW(path);
        }
    } cleanup{source};
    SendMessageW(item(ReceiveRole), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(item(SendRole), BM_SETCHECK, BST_CHECKED, 0);
    role();
    DWORD initialGdi = 0, initialUser = 0;
    for (unsigned run = 0; run < 7; ++run) {
        HANDLE file =
            CreateFileW(source, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open sender smoke fixture");
        std::vector<uint8_t> payload(run == 3 ? 0 : run == 4 ? 1 : 512 * 1024);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = uint8_t(i * 31 + run);
        DWORD written = 0;
        BOOL ok =
            payload.empty() || WriteFile(file, payload.data(), DWORD(payload.size()), &written, nullptr);
        CloseHandle(file);
        if (!ok || written != payload.size())
            throw std::runtime_error("Cannot write sender smoke fixture");
        auto path = std::wstring(source) + (run < 2 ? L".missing" : L"");
        SetWindowTextW(item(PathEdit), path.c_str());
        app.smokeFault = run == 2;
        SendMessageW(app.window, WM_COMMAND, StartButton, 0);
        if (!app.running)
            throw std::runtime_error("Sender smoke did not launch a worker");
        SendMessageW(app.window, WM_COMMAND, StartButton, 0); // Ignore a double click while running.
        auto deadline = Clock::now() + std::chrono::seconds(15);
        while (app.running && Clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(1);
        }
        bool failed = run < 3;
        if (app.running || app.worker.joinable() || !IsWindowEnabled(item(StartButton)) ||
            IsWindowEnabled(item(StopButton)) || app.lastError.empty() == failed ||
            app.smokeFrames != (run < 2    ? 0u
                                : run == 2 ? 1u
                                           : 3u))
            throw std::runtime_error("Sender failure/stop did not restore a restartable GUI");
        if (run == 2 && app.lastError.find(L"render_frame") == std::wstring::npos)
            throw std::runtime_error("Non-standard sender exception lost the failing stage");
        if (run == 3) {
            initialGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            initialUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        } else if (run > 3 && (GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) > initialGdi ||
                               GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) > initialUser))
            throw std::runtime_error("Repeated sender starts leaked GDI or window resources");
    }
    return true;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    cst::diagnostics::install();
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common);
    try {
        cst::diagnostics::stage("configure_opencv");
        cst::initializeImageRuntime(); // Before any OpenCV work or transfer thread.
        cst::diagnostics::append("image_runtime threads=1 gdi_conversion=sequential\r\n");
        wchar_t desktop[MAX_PATH]{};
        if (SUCCEEDED(
                SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop)))
            app.directory = std::wstring(desktop) + L"\\CloudScreenReceived";
        else
            app.directory = L"CloudScreenReceived";
        int count = 0;
        wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (args) {
            try {
                for (int i = 1; i < count; ++i) {
                    std::wstring option = args[i];
                    if (option == L"--smoke-test")
                        app.smoke = true;
                    else if (option == L"--sender-smoke-test") {
                        app.smoke = true;
                        app.senderSmoke = true;
                    } else if (option == L"--settings-smoke-test") {
                        app.smoke = true;
                        app.settingsSmoke = true;
                    } else if (option == L"--receive" && i + 1 < count) {
                        app.autoReceive = true;
                        app.directory = args[++i];
                    } else if (option == L"--process" && i + 1 < count) {
                        app.process = args[++i];
                        app.processOverride = true;
                    } else if (option == L"--config" && i + 1 < count)
                        app.settingsPath = args[++i];
                    else
                        throw std::runtime_error("Unknown startup option");
                }
            } catch (...) {
                LocalFree(args);
                throw;
            }
            LocalFree(args);
        }
        struct FixtureCleanup {
            std::wstring path;
            ~FixtureCleanup() {
                if (!path.empty())
                    DeleteFileW(path.c_str());
            }
        } fixture;
        if (app.settingsSmoke) {
            wchar_t folder[32768]{};
            DWORD n = GetCurrentDirectoryW(DWORD(std::size(folder)), folder);
            if (!n || n >= std::size(folder))
                throw std::runtime_error("Cannot create settings smoke fixture");
            fixture.path = std::wstring(folder) + L"\\cst-gui-settings-" +
                           std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
                           L".ini";
            cst::Settings s;
            s.send = true;
            s.source = L"C:\\fixtures\\中文文件.bin";
            s.directory = L"C:\\fixtures\\接收目录";
            s.process = L"SavedVDI.exe";
            s.encoding = {900, 500, 3, 8, 64, 64};
            s.sendFps = 8;
            s.captureFps = 30;
            s.restoreMouse = true;
            cst::saveSettings(fixture.path, s);
            app.settingsPath = fixture.path;
        }
        if (!app.smoke || app.settingsSmoke) {
            if (app.settingsPath.empty())
                app.settingsPath = cst::defaultSettingsPath();
            app.settings = cst::loadSettings(app.settingsPath, app.settingsWarning);
            app.source = app.settings.source;
            if (!app.autoReceive && !app.settings.directory.empty())
                app.directory = app.settings.directory;
            if (!app.processOverride)
                app.process = app.settings.process;
            if (app.autoReceive)
                app.settings.send = false;
        }
        WNDCLASSW cls{};
        cls.lpfnWndProc = procedure;
        cls.hInstance = instance;
        cls.lpszClassName = L"CloudScreenTransferMain";
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (!RegisterClassW(&cls))
            throw std::runtime_error("Cannot register window");
        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        int height = std::min(680, std::max(650, int(workArea.bottom - workArea.top)));
        int x = workArea.left + std::max(0, (int(workArea.right - workArea.left) - 900) / 2);
        int y = workArea.top + std::max(0, (int(workArea.bottom - workArea.top) - height) / 2);
        int width = 900;
        if (app.settings.windowSaved) {
            RECT previous{app.settings.windowX, app.settings.windowY,
                          app.settings.windowX + app.settings.windowWidth,
                          app.settings.windowY + app.settings.windowHeight};
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&previous, MONITOR_DEFAULTTONEAREST), &monitor))
                workArea = monitor.rcWork;
            width = std::max(900, std::min(app.settings.windowWidth, int(workArea.right - workArea.left)));
            height = std::max(650, std::min(app.settings.windowHeight, int(workArea.bottom - workArea.top)));
            x = std::clamp(app.settings.windowX, int(workArea.left),
                           std::max(int(workArea.left), int(workArea.right) - width));
            y = std::clamp(app.settings.windowY, int(workArea.top),
                           std::max(int(workArea.top), int(workArea.bottom) - height));
        }
        HWND window = CreateWindowW(cls.lpszClassName, L"CloudScreenTransfer 0.2.0", WS_OVERLAPPEDWINDOW, x,
                                    y, width, height, nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("Cannot create window");
        app.initializing = false;
        if (!app.settingsWarning.empty())
            log(app.settingsWarning);
        if (app.smoke) {
            bool valid = app.settingsSmoke
                             ? sender() && textOf(item(PathEdit)) == app.settings.source &&
                                   textOf(item(ProcessEdit)) == L"SavedVDI.exe" &&
                                   app.settings.encoding == cst::Encoding{900, 500, 3, 8, 64, 64} &&
                                   app.settings.sendFps == 8 &&
                                   cst::advancedSettingsSmoke(app.window, app.font, app.settings)
                         : app.senderSmoke ? senderSmoke()
                                           : smoke();
            app.smokeResult = valid ? 0 : 1;
            DestroyWindow(window);
        } else {
            ShowWindow(window, show);
            UpdateWindow(window);
            if (app.autoReceive)
                PostMessageW(window, WM_COMMAND, StartButton, 0);
        }
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (app.worker.joinable())
            app.worker.join();
        if (app.font)
            DeleteObject(app.font);
        if (app.heading)
            DeleteObject(app.heading);
        CoUninitialize();
        return app.smoke ? app.smokeResult : 0;
    } catch (...) {
        app.stopped = true;
        if (app.worker.joinable())
            app.worker.join();
        bool ignored = false;
        auto error = describeFailure(std::current_exception(), ignored);
        if (!app.smoke)
            MessageBoxW(nullptr, error.c_str(), L"启动失败", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
}
