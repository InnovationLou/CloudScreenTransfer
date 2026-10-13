/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "settings.h"
#include <windows.h>
#include <shlobj.h>
#include <array>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cst {
namespace {
[[noreturn]] void failure(const char *action) {
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
void cleanText(const std::wstring &text) {
    if (text.size() > 32760 || text.find_first_of(L"\r\n") != std::wstring::npos ||
        text.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid settings text");
}
std::wstring read(const std::wstring &path, const wchar_t *section, const wchar_t *name,
                  const std::wstring &fallback) {
    std::vector<wchar_t> buffer(32768);
    DWORD n = GetPrivateProfileStringW(section, name, fallback.c_str(), buffer.data(), DWORD(buffer.size()),
                                       path.c_str());
    return std::wstring(buffer.data(), n);
}
int number(const std::wstring &path, const wchar_t *section, const wchar_t *name, int fallback, int low,
           int high, std::wstring &warning) {
    auto value = read(path, section, name, std::to_wstring(fallback));
    try {
        size_t used = 0;
        int result = std::stoi(value, &used);
        if (used == value.size() && result >= low && result <= high)
            return result;
    } catch (const std::exception &) {
    }
    warning = L"部分保存参数无效，已恢复推荐值。";
    return fallback;
}
} // namespace
void Settings::validate() const {
    encoding.validate();
    if (sendFps < 1 || sendFps > 30 || captureFps < 1 || captureFps > 60 || progressAckMs < 500 ||
        progressAckMs > 30000 || controlAckMs < 250 || controlAckMs > 10000 || keyHoldMs < 1 ||
        keyHoldMs > 200 || keyGapMs < 1 || keyGapMs > 200 || imageFreshMs < 100 || imageFreshMs > 5000 ||
        unsigned(keyMode) > 2)
        throw std::invalid_argument("Timing/input values are outside the supported ranges");
    if (feedbackEnabled && imageFreshMs < (1000 + captureFps - 1) / captureFps + 100)
        throw std::invalid_argument("Image validity must cover the capture interval plus 100ms");
    cleanText(source);
    cleanText(directory);
    cleanText(process);
    if (windowWidth < 900 || windowWidth > 8000 || windowHeight < 650 || windowHeight > 8000)
        throw std::invalid_argument("Invalid saved window size");
}
std::wstring defaultSettingsPath() {
    std::array<wchar_t, 32768> buffer{};
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (!n || n >= buffer.size())
        failure("Locate settings directory");
    std::wstring path(buffer.data(), n);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"CloudScreenTransfer.ini";
    HANDLE probe = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return path;
    }
    wchar_t directory[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, directory)))
        failure("Locate per-user settings directory");
    std::wstring folder = std::wstring(directory) + L"\\CloudScreenTransfer";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        failure("Create settings directory");
    return folder + L"\\CloudScreenTransfer.ini";
}
Settings loadSettings(const std::wstring &path, std::wstring &warning) {
    Settings s;
    warning.clear();
    s.source = read(path, L"Main", L"Source", L"");
    s.directory = read(path, L"Main", L"Directory", L"");
    s.process = read(path, L"Main", L"Process", s.process);
    s.send = number(path, L"Main", L"Send", 0, 0, 1, warning) != 0;
    auto get = [&](const wchar_t *section, const wchar_t *key, unsigned &value, int low, int high) {
        value = unsigned(number(path, section, key, int(value), low, high, warning));
    };
    get(L"Encoding", L"Width", s.encoding.width, 900, 1920);
    get(L"Encoding", L"Height", s.encoding.height, 300, 1080);
    get(L"Encoding", L"Cell", s.encoding.cell, 2, 8);
    get(L"Encoding", L"Levels", s.encoding.levels, 2, 16);
    get(L"Encoding", L"Parity", s.encoding.parity, 16, 64);
    get(L"Encoding", L"WindowFrames", s.encoding.windowFrames, 16, 256);
    try {
        s.encoding.validate();
    } catch (const std::exception &) {
        s.encoding = Encoding{};
        warning = L"保存的编码组合无效，已恢复推荐编码。";
    }
    get(L"Timing", L"SendFps", s.sendFps, 1, 30);
    get(L"Timing", L"CaptureFps", s.captureFps, 1, 60);
    get(L"Timing", L"ProgressAckMs", s.progressAckMs, 500, 30000);
    get(L"Timing", L"ControlAckMs", s.controlAckMs, 250, 10000);
    get(L"Timing", L"KeyHoldMs", s.keyHoldMs, 1, 200);
    get(L"Timing", L"KeyGapMs", s.keyGapMs, 1, 200);
    get(L"Timing", L"ImageFreshMs", s.imageFreshMs, 100, 5000);
    s.keyMode = FeedbackKeyMode(number(path, L"Feedback", L"KeyMode", 0, 0, 2, warning));
    s.feedbackEnabled = number(path, L"Feedback", L"Enabled", 1, 0, 1, warning) != 0;
    s.clickCanvas = number(path, L"Feedback", L"ClickCanvas", 1, 0, 1, warning) != 0;
    s.restoreMouse = number(path, L"Feedback", L"RestoreMouse", 0, 0, 1, warning) != 0;
    s.windowSaved = number(path, L"Window", L"Saved", 0, 0, 1, warning) != 0;
    s.windowX = number(path, L"Window", L"X", 0, -100000, 100000, warning);
    s.windowY = number(path, L"Window", L"Y", 0, -100000, 100000, warning);
    s.windowWidth = number(path, L"Window", L"Width", 900, 900, 8000, warning);
    s.windowHeight = number(path, L"Window", L"Height", 680, 650, 8000, warning);
    if (s.feedbackEnabled && s.imageFreshMs < (1000 + s.captureFps - 1) / s.captureFps + 100) {
        s.imageFreshMs = (1000 + s.captureFps - 1) / s.captureFps + 100;
        warning = L"画面有效时间过短，已延长以覆盖抓取间隔。";
    }
    s.validate();
    return s;
}
void saveSettings(const std::wstring &path, const Settings &s) {
    s.validate();
    std::wostringstream out;
    out << L"[Main]\r\nSend=" << s.send << L"\r\nSource=" << s.source << L"\r\nDirectory=" << s.directory
        << L"\r\nProcess=" << s.process << L"\r\n[Encoding]\r\nWidth=" << s.encoding.width << L"\r\nHeight="
        << s.encoding.height << L"\r\nCell=" << s.encoding.cell << L"\r\nLevels=" << s.encoding.levels
        << L"\r\nParity=" << s.encoding.parity << L"\r\nWindowFrames=" << s.encoding.windowFrames
        << L"\r\n[Timing]\r\nSendFps=" << s.sendFps << L"\r\nCaptureFps=" << s.captureFps
        << L"\r\nProgressAckMs=" << s.progressAckMs << L"\r\nControlAckMs=" << s.controlAckMs
        << L"\r\nKeyHoldMs=" << s.keyHoldMs << L"\r\nKeyGapMs=" << s.keyGapMs << L"\r\nImageFreshMs="
        << s.imageFreshMs << L"\r\n[Feedback]\r\nEnabled=" << s.feedbackEnabled << L"\r\nClickCanvas="
        << s.clickCanvas << L"\r\nRestoreMouse=" << s.restoreMouse << L"\r\nKeyMode=" << unsigned(s.keyMode)
        << L"\r\n[Window]\r\nSaved=" << s.windowSaved << L"\r\nX=" << s.windowX << L"\r\nY=" << s.windowY
        << L"\r\nWidth=" << s.windowWidth << L"\r\nHeight=" << s.windowHeight << L"\r\n";
    const auto content = out.str();
    static std::atomic<unsigned> counter{0};
    const auto temporary =
        path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++counter) + L".new";
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        failure("Create settings temporary file");
    try {
        WORD bom = 0xfeff;
        DWORD written = 0, size = DWORD(content.size() * sizeof(wchar_t));
        if (!WriteFile(file, &bom, sizeof(bom), &written, nullptr) || written != sizeof(bom) ||
            !WriteFile(file, content.data(), size, &written, nullptr) || written != size ||
            !FlushFileBuffers(file))
            failure("Write settings");
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            failure("Commit settings");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr,
                                   path.c_str()); // Invalidate the profile API cache.
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw;
    }
}
} // namespace cst
