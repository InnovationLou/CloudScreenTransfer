/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include "encoding.h"
#include <string>

namespace cst {
enum class FeedbackKeyMode : unsigned { Scan, VirtualKey, Unicode };
struct Settings {
    Encoding encoding;
    unsigned sendFps = 5, captureFps = 20, progressAckMs = 4000, controlAckMs = 1000;
    unsigned keyHoldMs = 8, keyGapMs = 12, imageFreshMs = 900;
    FeedbackKeyMode keyMode = FeedbackKeyMode::Scan;
    bool feedbackEnabled = true, clickCanvas = true, restoreMouse = false, send = false;
    std::wstring source, directory, process = L"uSmartView_VDI_Client.exe";
    bool windowSaved = false;
    int windowX = 0, windowY = 0, windowWidth = 900, windowHeight = 680;
    void validate() const;
};
std::wstring defaultSettingsPath();
Settings loadSettings(const std::wstring &path, std::wstring &warning);
void saveSettings(const std::wstring &path, const Settings &settings);
} // namespace cst
