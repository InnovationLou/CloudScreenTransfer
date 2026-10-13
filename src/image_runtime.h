/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include <opencv2/core.hpp>

namespace cst {
// Call once at process startup, before any OpenCV operation or worker starts.
// Changing this process-wide setting while image operations run is unsafe.
void initializeImageRuntime();

// Sequential, exact grayscale conversion for the Win7 GDI display path.
void grayToBgra(const cv::Mat &gray, cv::Mat &bgra);
} // namespace cst
