/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "image_runtime.h"
#include <stdexcept>

namespace cst {
void initializeImageRuntime() {
    // Match libcimbar's cloud-safe sender and screen probe. The static OpenCV
    // package uses Microsoft Concurrency; static CRT and a Win7 import table
    // alone do not make that scheduler usable in a restricted cloud desktop.
    cv::setNumThreads(1);
}

void grayToBgra(const cv::Mat &gray, cv::Mat &bgra) {
    if (gray.empty() || gray.type() != CV_8UC1 || gray.dims != 2)
        throw std::invalid_argument("GDI conversion expects a nonempty grayscale image");
    bgra.create(gray.rows, gray.cols, CV_8UC4);
    for (int row = 0; row < gray.rows; ++row) {
        const auto *source = gray.ptr<uint8_t>(row);
        auto *target = bgra.ptr<cv::Vec4b>(row);
        for (int col = 0; col < gray.cols; ++col)
            target[col] = cv::Vec4b(source[col], source[col], source[col], 255);
    }
}
} // namespace cst
