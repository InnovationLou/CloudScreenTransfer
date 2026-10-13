/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "common.h"
#include "settings.h"
#include <opencv2/core.hpp>
#include <memory>
namespace cst {
HWND findClient(const std::wstring &executableName);
class Capture {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Capture();
    ~Capture();
    cv::Mat take(HWND target);
};
class Canvas {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit Canvas(std::function<void(const Feedback &)> feedback, bool visible = true,
                    Encoding encoding = {});
    ~Canvas();
    void display(const cv::Mat &gray);
    bool poll();
};
// Inject only while the captured cloud client is foreground, its canvas is
// visible, no modifiers are held, and the caller's fresh-frame check succeeds.
bool sendFeedback(HWND target, const cv::Rect &canvas, const std::string &wire, Cancel interrupted,
                  std::wstring &failure, const Settings &settings = {});
} // namespace cst
