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
constexpr int DataX = 24, DataY = 84, HeaderColumns = 64, HeaderCell = 4;
using Header = std::array<uint8_t, 40>;
using Stream = std::vector<uint8_t>;
void store16(uint8_t *p, unsigned n) {
    p[0] = uint8_t(n);
    p[1] = uint8_t(n >> 8);
}
void store32(uint8_t *p, uint32_t n) {
    for (int i = 0; i < 4; ++i)
        p[i] = uint8_t(n >> (8 * i));
}
void store64(uint8_t *p, uint64_t n) {
    for (int i = 0; i < 8; ++i)
        p[i] = uint8_t(n >> (8 * i));
}
unsigned load16(const uint8_t *p) {
    return p[0] | unsigned(p[1]) << 8;
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
    info.encoding.validate();
    if (!info.transfer)
        throw std::invalid_argument("Frame transfer ID must be nonzero");
    const auto &e = info.encoding;
    Header b{};
    b[0] = 'C';
    b[1] = 'S';
    b[2] = 'T';
    b[3] = e.legacy() ? '1' : '2';
    b[4] = e.legacy() ? 1 : 2;
    b[5] = uint8_t((info.complete ? 1 : 0) | (e.legacy() ? 0 : e.bits() << 1));
    b[6] = uint8_t(e.parity);
    b[7] = uint8_t(e.cell);
    store64(b.data() + 8, info.transfer);
    store32(b.data() + 16, info.sequence);
    store64(b.data() + 20, info.window);
    if (e.legacy())
        store32(b.data() + 28, crc32(b.data(), 28));
    else {
        store16(b.data() + 28, e.width);
        store16(b.data() + 30, e.height);
        store16(b.data() + 32, e.windowFrames);
        store32(b.data() + 36, crc32(b.data(), 36));
    }
    return b;
}
bool decodeHeader(const Header &b, FrameInfo &info) {
    if (b[0] != 'C' || b[1] != 'S' || b[2] != 'T' || !load64(b.data() + 8))
        return false;
    Encoding e;
    if (b[3] == '1' && b[4] == 1) {
        if ((b[5] & ~1u) || b[6] != 48 || b[7] != 2 || load32(b.data() + 28) != crc32(b.data(), 28))
            return false;
    } else if (b[3] == '2' && b[4] == 2) {
        if ((b[5] & ~15u) || ((b[5] >> 1) & 7) < 1 || ((b[5] >> 1) & 7) > 4 || b[34] || b[35] ||
            load32(b.data() + 36) != crc32(b.data(), 36))
            return false;
        e.width = load16(b.data() + 28);
        e.height = load16(b.data() + 30);
        e.windowFrames = load16(b.data() + 32);
        e.parity = b[6];
        e.cell = b[7];
        e.levels = 1u << ((b[5] >> 1) & 7);
        try {
            e.validate();
        } catch (const std::invalid_argument &) {
            return false;
        }
    } else
        return false;
    info.transfer = load64(b.data() + 8);
    info.sequence = load32(b.data() + 16);
    info.window = load64(b.data() + 20);
    info.complete = (b[5] & 1) != 0;
    info.encoding = e;
    return true;
}
void paintHeader(cv::Mat &image, const Header &header, bool legacy) {
    for (int copy = 0; copy < 3; ++copy)
        for (int bit = 0; bit < (legacy ? 32 : 40) * 8; ++bit) {
            int white = (header[bit / 8] >> (7 - bit % 8)) & 1;
            image(cv::Rect(24 + copy * 272 + bit % HeaderColumns * HeaderCell,
                           28 + bit / HeaderColumns * HeaderCell, HeaderCell, HeaderCell))
                .setTo(white * 255);
        }
}
bool readHeader(const cv::Mat &image, FrameInfo &info) {
    if (image.cols < 824 || image.rows < 48)
        return false;
    Header accepted{};
    bool found = false;
    for (int copy = 0; copy < 3; ++copy) {
        Header bytes{};
        // The extra row is ignored when checking a legacy header.
        for (int bit = 0; bit < 40 * 8; ++bit)
            if (cv::mean(image(cv::Rect(24 + copy * 272 + bit % HeaderColumns * HeaderCell,
                                        28 + bit / HeaderColumns * HeaderCell, HeaderCell, HeaderCell)))[0] >
                127.5)
                bytes[bit / 8] |= uint8_t(1u << (7 - bit % 8));
        FrameInfo trial;
        if (!decodeHeader(bytes, trial))
            continue;
        if (found &&
            !std::equal(bytes.begin(), bytes.begin() + (trial.encoding.legacy() ? 32 : 40), accepted.begin()))
            return false;
        accepted = bytes;
        info = trial;
        found = true;
    }
    return found;
}
double borderScore(const cv::Mat &crop) {
    int w = crop.cols, h = crop.rows;
    return cv::mean(crop(cv::Rect(16, 16, w - 32, 4)))[0] +
           cv::mean(crop(cv::Rect(16, h - 20, w - 32, 4)))[0] +
           cv::mean(crop(cv::Rect(16, 20, 4, h - 40)))[0] +
           cv::mean(crop(cv::Rect(w - 20, 20, 4, h - 40)))[0] -
           cv::mean(crop(cv::Rect(20, 20, w - 40, 4)))[0] -
           cv::mean(crop(cv::Rect(20, h - 24, w - 40, 4)))[0] -
           cv::mean(crop(cv::Rect(20, 24, 4, h - 48)))[0] -
           cv::mean(crop(cv::Rect(w - 24, 24, 4, h - 48)))[0];
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
        if (box.width < int(MinCanvasWidth) - 34 || box.width > 1890 ||
            box.height < int(MinCanvasHeight) - 34 || box.height > 1050)
            continue;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int x = box.x - 16 + dx, y = box.y - 16 + dy;
                if (x < 0 || y < 0 || x >= gray.cols || y >= gray.rows)
                    continue;
                FrameInfo trial;
                if (!readHeader(gray(cv::Rect(x, y, gray.cols - x, gray.rows - y)), trial))
                    continue;
                const auto &e = trial.encoding;
                if (std::abs(box.width - int(e.width - 32)) > 2 ||
                    std::abs(box.height - int(e.height - 32)) > 2)
                    continue;
                cv::Rect candidate(x, y, int(e.width), int(e.height));
                if ((candidate & cv::Rect(0, 0, gray.cols, gray.rows)) != candidate)
                    continue;
                double score = borderScore(gray(candidate));
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
cv::Rect paletteRect(const Encoding &e, unsigned level, bool sample) {
    int step = e.legacy() ? 59 : int((e.width - 48) / e.levels);
    return sample ? cv::Rect(28 + int(level) * step, 56, step - 12, 16)
                  : cv::Rect(24 + int(level) * step, 52, step - 4, 24);
}
unsigned grayInverse(unsigned n) {
    unsigned value = n;
    while ((n >>= 1) != 0)
        value ^= n;
    return value;
}
void paintStream(cv::Mat &image, const Stream &bytes, const Encoding &e) {
    unsigned columns = (e.width - 48) / e.cell, rows = (e.height - 108) / e.cell, bits = e.bits();
    size_t cell = 0;
    for (unsigned row = 0; row < rows; ++row)
        for (unsigned col = 0; col < columns; ++col, ++cell) {
            unsigned symbol = 0;
            for (unsigned b = 0; b < bits; ++b) {
                size_t at = cell * bits + b;
                symbol = (symbol << 1) | (at < bytes.size() * 8 ? (bytes[at / 8] >> (7 - at % 8)) & 1u : 0u);
            }
            uint8_t value = uint8_t(32 + grayInverse(symbol) * 192 / (e.levels - 1));
            for (unsigned yy = 0; yy < e.cell; ++yy)
                std::fill_n(image.ptr<uint8_t>(DataY + row * e.cell + yy) + DataX + col * e.cell, e.cell,
                            value);
        }
}
bool readStream(const cv::Mat &image, Stream &bytes, const Encoding &e) {
    std::vector<double> palette(e.levels);
    for (unsigned level = 0; level < e.levels; ++level) {
        palette[level] = cv::mean(image(paletteRect(e, level, true)))[0];
        if (level && palette[level] - palette[level - 1] < 4)
            return false;
    }
    unsigned columns = (e.width - 48) / e.cell, rows = (e.height - 108) / e.cell, bits = e.bits();
    size_t cell = 0;
    for (unsigned row = 0; row < rows && cell * bits < bytes.size() * 8; ++row)
        for (unsigned col = 0; col < columns && cell * bits < bytes.size() * 8; ++col, ++cell) {
            unsigned sum = 0;
            for (unsigned yy = 0; yy < e.cell; ++yy) {
                const auto *p = image.ptr<uint8_t>(DataY + row * e.cell + yy) + DataX + col * e.cell;
                for (unsigned xx = 0; xx < e.cell; ++xx)
                    sum += p[xx];
            }
            double value = double(sum) / (e.cell * e.cell);
            unsigned level = 0;
            for (unsigned i = 1; i < e.levels; ++i)
                if (std::abs(value - palette[i]) < std::abs(value - palette[level]))
                    level = i;
            unsigned symbol = level ^ (level >> 1);
            for (unsigned b = 0; b < bits; ++b) {
                size_t at = cell * bits + b;
                if (at < bytes.size() * 8)
                    bytes[at / 8] |= uint8_t(((symbol >> (bits - 1 - b)) & 1u) << (7 - at % 8));
            }
        }
    return true;
}
} // namespace
struct FrameCodec::Impl {
    correct_reed_solomon *rs = nullptr;
    unsigned parity = 0;
    correct_reed_solomon *get(unsigned wanted) {
        if (wanted != parity) {
            if (rs)
                correct_reed_solomon_destroy(rs);
            rs = correct_reed_solomon_create(correct_rs_primitive_polynomial_8_7_2_1_0, 1, 1, wanted);
            parity = wanted;
        }
        if (!rs)
            throw std::runtime_error("Cannot create Reed-Solomon codec");
        return rs;
    }
    ~Impl() {
        if (rs)
            correct_reed_solomon_destroy(rs);
    }
};
FrameCodec::FrameCodec() : impl(new Impl) {}
FrameCodec::~FrameCodec() = default;
cv::Mat FrameCodec::encode(const WireFrame &frame) {
    const auto &e = frame.info.encoding;
    auto header = encodeHeader(frame.info);
    if (frame.packets.size() != e.slots())
        throw std::invalid_argument("Frame packet count differs from encoding");
    Stream stream(size_t(e.slots()) * CodeBytes, 0);
    auto *rs = impl->get(e.parity);
    for (unsigned slot = 0; slot < e.slots(); ++slot) {
        auto message = encodePacket(frame.packets[slot], e);
        std::array<uint8_t, CodeBytes> code{};
        if (correct_reed_solomon_encode(rs, message.data(), message.size(), code.data()) != CodeBytes)
            throw std::runtime_error("RS frame encode failed");
        for (unsigned byte = 0; byte < CodeBytes; ++byte)
            stream[size_t(byte) * e.slots() + slot] = code[byte];
    }
    cv::Mat image(e.height, e.width, CV_8UC1, cv::Scalar(0));
    image(cv::Rect(16, 16, e.width - 32, e.height - 32)).setTo(255);
    image(cv::Rect(20, 20, e.width - 40, e.height - 40)).setTo(0);
    paintHeader(image, header, e.legacy());
    for (unsigned level = 0; level < e.levels; ++level)
        image(paletteRect(e, level, false)).setTo(32 + level * 192 / (e.levels - 1));
    paintStream(image, stream, e);
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
    const auto &e = result.frame.info.encoding;
    Stream stream(size_t(e.slots()) * CodeBytes, 0);
    if (!readStream(gray(result.canvas), stream, e)) {
        result.status = "palette_collapsed";
        return result;
    }
    result.valid = true;
    auto *rs = impl->get(e.parity);
    for (unsigned slot = 0; slot < e.slots(); ++slot) {
        std::array<uint8_t, CodeBytes> code{};
        for (unsigned byte = 0; byte < CodeBytes; ++byte)
            code[byte] = stream[size_t(byte) * e.slots() + slot];
        std::vector<uint8_t> message(e.messageBytes());
        Packet packet;
        if (correct_reed_solomon_decode(rs, code.data(), code.size(), message.data()) == e.messageBytes() &&
            decodePacket(message.data(), message.size(), packet, e) &&
            packet.transfer == result.frame.info.transfer)
            result.frame.packets.push_back(std::move(packet));
    }
    result.status = result.frame.packets.empty() ? "payload_failed" : "ok";
    return result;
}
} // namespace cst
