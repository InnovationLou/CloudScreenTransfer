/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include "common.h"
#include <opencv2/core.hpp>
#include <memory>
namespace cst {
struct DecodeResult {
    bool valid = false;
    DecodedFrame frame;
    cv::Rect canvas;
    std::string status;
};
class FrameCodec {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    FrameCodec();
    ~FrameCodec();
    FrameCodec(const FrameCodec &) = delete;
    cv::Mat encode(const WireFrame &frame);
    DecodeResult decode(const cv::Mat &captured);
};
} // namespace cst
