/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "frame_codec.h"
#include "protocol.h"
extern "C" {
#include <correct.h>
}
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace cst {
namespace {
constexpr int DataX = 24, DataY = 84, DataWidth = 952, DataHeight = 492;
constexpr int Cell = 2, Columns = DataWidth / Cell, Rows = DataHeight / Cell;
constexpr int HeaderBytes = 32, HeaderColumns = 64, HeaderCell = 4;
constexpr int BorderX = 16, BorderY = 16, BorderWidth = 968, BorderHeight = 568;
constexpr size_t StreamBytes = size_t(Slots) * CodeBytes;
constexpr std::array<uint8_t, 4> Luminance{{32, 96, 160, 224}};
using Header = std::array<uint8_t, HeaderBytes>;
using Stream = std::array<uint8_t, StreamBytes>;
static_assert(Slots == 114 && CodeBytes == 255 && MessageBytes == 207 && ParityBytes == 48,
              "The wire geometry is versioned; changing it requires a new frame format");
static_assert(size_t(Columns) * Rows / 4 >= StreamBytes, "Optical payload does not fit");

uint32_t frameCrc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = ~uint32_t(0);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
void store32(uint8_t *p, uint32_t n) {
    for (int i = 0; i < 4; ++i)
        p[i] = uint8_t(n >> (8 * i));
}
void store64(uint8_t *p, uint64_t n) {
    for (int i = 0; i < 8; ++i)
        p[i] = uint8_t(n >> (8 * i));
}
uint32_t load32(const uint8_t *p) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n |= uint32_t(p[i]) << (8 * i);
    return n;
}
uint64_t load64(const uint8_t *p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n |= uint64_t(p[i]) << (8 * i);
    return n;
}
Header encodeHeader(const FrameInfo &info) {
    if (!info.transfer)
        throw std::invalid_argument("Frame transfer ID must be nonzero");
    Header b{};
    b[0] = 'C';
    b[1] = 'S';
    b[2] = 'T';
    b[3] = '1';
    b[4] = 1;
    b[5] = info.complete ? 1 : 0;
    b[6] = 48;
    b[7] = 2;
    store64(b.data() + 8, info.transfer);
    store32(b.data() + 16, info.sequence);
    store64(b.data() + 20, info.window);
    store32(b.data() + 28, frameCrc32(b.data(), 28));
    return b;
}
bool decodeHeader(const Header &b, FrameInfo &info) {
    if (b[0] != 'C' || b[1] != 'S' || b[2] != 'T' || b[3] != '1' || b[4] != 1 || (b[5] & ~1u) || b[6] != 48 ||
        b[7] != 2 || load32(b.data() + 28) != frameCrc32(b.data(), 28) || !load64(b.data() + 8))
        return false;
    info.transfer = load64(b.data() + 8);
    info.sequence = load32(b.data() + 16);
    info.window = load64(b.data() + 20);
    info.complete = (b[5] & 1) != 0;
    return true;
}
void paintHeader(cv::Mat &image, const Header &header) {
    for (int copy = 0; copy < 3; ++copy)
        for (int bit = 0; bit < HeaderBytes * 8; ++bit) {
            int white = (header[bit / 8] >> (7 - bit % 8)) & 1;
            image(cv::Rect(24 + copy * 272 + (bit % HeaderColumns) * HeaderCell,
                           28 + (bit / HeaderColumns) * HeaderCell, HeaderCell, HeaderCell))
                .setTo(white * 255);
        }
}
bool readHeader(const cv::Mat &image, FrameInfo &info) {
    Header accepted{};
    bool found = false;
    for (int copy = 0; copy < 3; ++copy) {
        Header bytes{};
        for (int bit = 0; bit < HeaderBytes * 8; ++bit) {
            cv::Rect cell(24 + copy * 272 + (bit % HeaderColumns) * HeaderCell,
                          28 + (bit / HeaderColumns) * HeaderCell, HeaderCell, HeaderCell);
            if (cv::mean(image(cell))[0] > 127.5)
                bytes[bit / 8] |= uint8_t(1u << (7 - bit % 8));
        }
        FrameInfo trial;
        if (!decodeHeader(bytes, trial))
            continue;
        // An invalid copy can be skipped; two different CRC-valid copies indicate a torn frame.
        if (found && bytes != accepted)
            return false;
        accepted = bytes;
        info = trial;
        found = true;
    }
    return found;
}
double borderScore(const cv::Mat &crop) {
    return cv::mean(crop(cv::Rect(16, 16, 968, 4)))[0] + cv::mean(crop(cv::Rect(16, 580, 968, 4)))[0] +
           cv::mean(crop(cv::Rect(16, 20, 4, 560)))[0] + cv::mean(crop(cv::Rect(980, 20, 4, 560)))[0] -
           cv::mean(crop(cv::Rect(20, 20, 960, 4)))[0] - cv::mean(crop(cv::Rect(20, 576, 960, 4)))[0] -
           cv::mean(crop(cv::Rect(20, 24, 4, 552)))[0] - cv::mean(crop(cv::Rect(976, 24, 4, 552)))[0];
}
bool findCanvas(const cv::Mat &gray, cv::Rect &position, FrameInfo &info) {
    cv::Mat mask;
    cv::threshold(gray, mask, 235, 255, cv::THRESH_BINARY);
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    double bestScore = -1e9;
    bool found = false;
    for (const auto &contour : contours) {
        auto box = cv::boundingRect(contour);
        // Only the native 1:1 outline is accepted. Small threshold uncertainty is
        // resolved with the header and the precise outline/gutter score below.
        if (std::abs(box.width - BorderWidth) > 2 || std::abs(box.height - BorderHeight) > 2)
            continue;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                cv::Rect candidate(box.x - BorderX + dx, box.y - BorderY + dy, CanvasWidth, CanvasHeight);
                if ((candidate & cv::Rect(0, 0, gray.cols, gray.rows)) != candidate)
                    continue;
                auto crop = gray(candidate);
                FrameInfo trial;
                if (!readHeader(crop, trial))
                    continue;
                double score = borderScore(crop);
                // Four-pixel header cells can survive a one-pixel shift. Selecting the
                // first valid header would misalign the two-pixel payload cells.
                if (score > bestScore) {
                    bestScore = score;
                    position = candidate;
                    info = trial;
                    found = true;
                }
            }
    }
    return found;
}
void paintStream(cv::Mat &image, const Stream &bytes) {
    size_t cell = 0;
    for (int row = 0; row < Rows; ++row) {
        auto upper = image.ptr<uint8_t>(DataY + row * Cell);
        auto lower = image.ptr<uint8_t>(DataY + row * Cell + 1);
        for (int col = 0; col < Columns; ++col, ++cell) {
            unsigned symbol = cell < StreamBytes * 4 ? (bytes[cell / 4] >> (6 - 2 * (cell % 4))) & 3u : 0;
            // Increasing physical levels are labelled 00,01,11,10.
            unsigned level = symbol ^ (symbol >> 1); // Two-bit Gray inverse equals Gray encoding.
            auto value = Luminance[level];
            upper[DataX + col * Cell] = upper[DataX + col * Cell + 1] = value;
            lower[DataX + col * Cell] = lower[DataX + col * Cell + 1] = value;
        }
    }
}
bool readStream(const cv::Mat &image, Stream &bytes) {
    std::array<double, 4> palette{};
    for (int level = 0; level < 4; ++level)
        palette[level] = cv::mean(image(cv::Rect(28 + level * 59, 56, 47, 16)))[0];
    for (int level = 1; level < 4; ++level)
        if (palette[level] - palette[level - 1] < 4)
            return false;
    size_t cell = 0;
    for (int row = 0; row < Rows && cell < StreamBytes * 4; ++row) {
        auto upper = image.ptr<uint8_t>(DataY + row * Cell);
        auto lower = image.ptr<uint8_t>(DataY + row * Cell + 1);
        for (int col = 0; col < Columns && cell < StreamBytes * 4; ++col, ++cell) {
            int x = DataX + col * Cell;
            double value = (unsigned(upper[x]) + upper[x + 1] + lower[x] + lower[x + 1]) * .25;
            int level = 0;
            for (int i = 1; i < 4; ++i)
                if (std::abs(value - palette[i]) < std::abs(value - palette[level]))
                    level = i;
            unsigned symbol = unsigned(level) ^ (unsigned(level) >> 1);
            bytes[cell / 4] |= uint8_t(symbol << (6 - 2 * (cell % 4)));
        }
    }
    return true;
}
} // namespace

struct FrameCodec::Impl {
    correct_reed_solomon *rs =
        correct_reed_solomon_create(correct_rs_primitive_polynomial_8_7_2_1_0, 1, 1, ParityBytes);
    Impl() {
        if (!rs)
            throw std::runtime_error("Cannot create RS(255,207) codec");
    }
    ~Impl() { correct_reed_solomon_destroy(rs); }
};
FrameCodec::FrameCodec() : impl(new Impl) {}
FrameCodec::~FrameCodec() = default;

cv::Mat FrameCodec::encode(const WireFrame &frame) {
    auto header = encodeHeader(frame.info);
    Stream stream{};
    for (unsigned slot = 0; slot < Slots; ++slot) {
        auto message = encodePacket(frame.packets[slot]);
        std::array<uint8_t, CodeBytes> code{};
        if (correct_reed_solomon_encode(impl->rs, message.data(), message.size(), code.data()) != CodeBytes)
            throw std::runtime_error("RS frame encode failed");
        for (unsigned byte = 0; byte < CodeBytes; ++byte)
            stream[size_t(byte) * Slots + slot] = code[byte];
    }
    cv::Mat image(CanvasHeight, CanvasWidth, CV_8UC1, cv::Scalar(0));
    image(cv::Rect(BorderX, BorderY, BorderWidth, BorderHeight)).setTo(255);
    image(cv::Rect(20, 20, 960, 560)).setTo(0);
    paintHeader(image, header);
    for (int level = 0; level < 4; ++level)
        image(cv::Rect(24 + level * 59, 52, 55, 24)).setTo(Luminance[level]);
    paintStream(image, stream);
    return image;
}

DecodeResult FrameCodec::decode(const cv::Mat &captured) {
    DecodeResult result;
    result.status = "border_missing_or_scaled";
    if (captured.empty() || captured.depth() != CV_8U ||
        (captured.channels() != 1 && captured.channels() != 3 && captured.channels() != 4)) {
        result.status = "invalid_image_format";
        return result;
    }
    cv::Mat gray;
    if (captured.channels() == 1)
        gray = captured;
    else
        cv::cvtColor(captured, gray, captured.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    if (!findCanvas(gray, result.canvas, result.frame.info))
        return result;
    Stream stream{};
    if (!readStream(gray(result.canvas), stream)) {
        result.status = "palette_collapsed";
        return result;
    }
    result.valid = true;
    for (unsigned slot = 0; slot < Slots; ++slot) {
        std::array<uint8_t, CodeBytes> code{};
        for (unsigned byte = 0; byte < CodeBytes; ++byte)
            code[byte] = stream[size_t(byte) * Slots + slot];
        std::array<uint8_t, MessageBytes> message{};
        Packet packet;
        if (correct_reed_solomon_decode(impl->rs, code.data(), code.size(), message.data()) == MessageBytes &&
            decodePacket(message, packet) && packet.transfer == result.frame.info.transfer)
            result.frame.packets.push_back(packet);
    }
    result.status = result.frame.packets.empty() ? "payload_failed" : "ok";
    return result;
}
} // namespace cst
