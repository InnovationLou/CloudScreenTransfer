/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "image_runtime.h"
#include <windows.h>
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace cst {
void imageRuntimeSelfTest() {
    if (cv::getNumThreads() != 1)
        throw std::runtime_error("Cloud image runtime must disable OpenCV parallel workers");
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            DWORD caller = GetCurrentThreadId();
            std::atomic<unsigned> calls{0};
            std::atomic<bool> sameThread{true};
            cv::parallel_for_(
                cv::Range(0, 100000),
                [&](const cv::Range &) {
                    ++calls;
                    if (GetCurrentThreadId() != caller)
                        sameThread = false;
                },
                128);
            if (!sameThread || calls != 1)
                throw std::runtime_error("OpenCV entered a parallel region on a transfer worker");
            // A padded input exercises row strides and every possible grayscale
            // value. Compare against the library reference and independently
            // round-trip to the original gray bytes.
            cv::Mat padded(603, 1009, CV_8UC1, cv::Scalar(17));
            cv::Mat gray = padded(cv::Rect(3, 2, 1000, 600));
            for (int row = 0; row < gray.rows; ++row)
                for (int col = 0; col < gray.cols; ++col)
                    gray.at<uint8_t>(row, col) = uint8_t(row * 31 + col);
            cv::Mat actual, reference, restored;
            grayToBgra(gray, actual);
            cv::cvtColor(gray, reference, cv::COLOR_GRAY2BGRA);
            cv::cvtColor(actual, restored, cv::COLOR_BGRA2GRAY);
            if (cv::norm(actual, reference, cv::NORM_INF) != 0 ||
                cv::norm(restored, gray, cv::NORM_INF) != 0 || !actual.isContinuous())
                throw std::runtime_error("Sequential GDI conversion changed protocol pixels");
            gray.setTo(42);
            if (cv::norm(actual, reference, cv::NORM_INF) != 0)
                throw std::runtime_error("Canvas pixels must own their storage");
            grayToBgra(gray, actual);
            if (actual.at<cv::Vec4b>(599, 999) != cv::Vec4b(42, 42, 42, 255))
                throw std::runtime_error("Reused GDI buffer kept stale pixels");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    std::cout << "Win7 image runtime: sequential worker dispatch and exact owned GDI pixels passed.\n";
}
} // namespace cst
