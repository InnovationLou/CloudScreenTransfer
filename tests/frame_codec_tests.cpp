/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "frame_codec.h"
#include "protocol.h"
extern "C" {
#include <correct.h>
}
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace cst {
namespace {
constexpr int DataX = 24, DataY = 84, Columns = 476;
constexpr std::array<uint8_t, 4> Levels{{32, 96, 160, 224}};
using Header = std::array<uint8_t, 32>;
using Code = std::array<uint8_t, CodeBytes>;
void check(bool condition, const char *name, unsigned &passed) {
    if (!condition)
        throw std::runtime_error(std::string("Frame codec self-test failed: ") + name);
    ++passed;
}
bool samePacket(const Packet &a, const Packet &b) {
    return a.transfer == b.transfer && a.index == b.index && a.data == b.data;
}
bool sameInfo(const FrameInfo &a, const FrameInfo &b) {
    return a.transfer == b.transfer && a.sequence == b.sequence && a.window == b.window &&
           a.complete == b.complete;
}
bool fullFrame(const DecodeResult &result, const WireFrame &expected) {
    if (!result.valid || !sameInfo(result.frame.info, expected.info) || result.frame.packets.size() != Slots)
        return false;
    for (unsigned i = 0; i < Slots; ++i)
        if (!samePacket(result.frame.packets[i], expected.packets[i]))
            return false;
    return true;
}
WireFrame unknownFrame(std::mt19937_64 &rng) {
    WireFrame frame;
    frame.info = {rng() | 1u, (uint64_t(1) << 43) + rng() % 1024, uint32_t(rng()), false};
    for (auto &packet : frame.packets) {
        packet.transfer = frame.info.transfer;
        packet.index = rng() % (uint64_t(1) << 43);
        for (auto &byte : packet.data)
            byte = uint8_t(rng());
    }
    return frame;
}
Header readHeader(const cv::Mat &image, int copy = 0) {
    Header b{};
    for (int bit = 0; bit < 256; ++bit)
        if (image.at<uint8_t>(28 + (bit / 64) * 4, 24 + copy * 272 + (bit % 64) * 4) > 127)
            b[bit / 8] |= uint8_t(1u << (7 - bit % 8));
    return b;
}
void writeHeader(cv::Mat &image, Header b, int copy = -1, bool refreshCrc = true) {
    if (refreshCrc) {
        auto crc = crc32(b.data(), 28);
        for (int i = 0; i < 4; ++i)
            b[28 + i] = uint8_t(crc >> (8 * i));
    }
    for (int c = 0; c < 3; ++c) {
        if (copy != -1 && copy != c)
            continue;
        for (int bit = 0; bit < 256; ++bit)
            image(cv::Rect(24 + c * 272 + (bit % 64) * 4, 28 + (bit / 64) * 4, 4, 4))
                .setTo(((b[bit / 8] >> (7 - bit % 8)) & 1) * 255);
    }
}
void paintByte(cv::Mat &image, size_t index, uint8_t byte) {
    for (unsigned piece = 0; piece < 4; ++piece) {
        auto symbol = (byte >> (6 - 2 * piece)) & 3u;
        auto level = symbol ^ (symbol >> 1);
        size_t cell = index * 4 + piece;
        image(cv::Rect(DataX + int(cell % Columns) * 2, DataY + int(cell / Columns) * 2, 2, 2))
            .setTo(Levels[level]);
    }
}
uint8_t readByte(const cv::Mat &image, size_t index) {
    uint8_t out = 0;
    for (unsigned piece = 0; piece < 4; ++piece) {
        size_t cell = index * 4 + piece;
        uint8_t value = image.at<uint8_t>(DataY + int(cell / Columns) * 2, DataX + int(cell % Columns) * 2);
        unsigned level = 0;
        while (level < 3 && value != Levels[level])
            ++level;
        out |= uint8_t((level ^ (level >> 1)) << (6 - 2 * piece));
    }
    return out;
}
Code getCode(const cv::Mat &image, unsigned slot) {
    Code code{};
    for (unsigned j = 0; j < CodeBytes; ++j)
        code[j] = readByte(image, size_t(j) * Slots + slot);
    return code;
}
void putCode(cv::Mat &image, unsigned slot, const Code &code) {
    for (unsigned j = 0; j < CodeBytes; ++j)
        paintByte(image, size_t(j) * Slots + slot, code[j]);
}
struct Rs {
    correct_reed_solomon *p =
        correct_reed_solomon_create(correct_rs_primitive_polynomial_8_7_2_1_0, 1, 1, 48);
    Rs() {
        if (!p)
            throw std::runtime_error("Test RS allocation failed");
    }
    ~Rs() { correct_reed_solomon_destroy(p); }
    Code encode(const PacketWire &message) {
        Code out{};
        if (correct_reed_solomon_encode(p, message.data(), message.size(), out.data()) != CodeBytes)
            throw std::runtime_error("Test RS encode failed");
        return out;
    }
};
} // namespace

void frameCodecSelfTest() {
    unsigned passed = 0;
    std::mt19937_64 rng(0x143d984a8e19883ull); // Unknown fixture bytes; decoder receives only pixels.
    FrameCodec codec;
    Rs rs;
    auto frame = unknownFrame(rng);
    auto image = codec.encode(frame);
    auto result = codec.decode(image);
    check(image.type() == CV_8UC1 && image.cols == 1000 && image.rows == 600, "native grayscale dimensions",
          passed);
    check(fullFrame(result, frame), "114 arbitrary real packets roundtrip", passed);
    check(result.canvas == cv::Rect(0, 0, 1000, 600), "exact ROI alignment", passed);
    for (unsigned i = 0; i < 3; ++i) {
        auto next = unknownFrame(rng);
        check(fullFrame(codec.decode(codec.encode(next)), next), "independent unknown payload roundtrip",
              passed);
    }

    bool interleaving = true;
    for (unsigned slot = 0; slot < Slots; ++slot)
        interleaving = interleaving && getCode(image, slot) == rs.encode(encodePacket(frame.packets[slot]));
    check(interleaving, "all 29070 bytes follow column-wise block interleave", passed);
    bool grayAdjacent = true;
    for (unsigned level = 0; level < 3; ++level) {
        unsigned difference = (level ^ (level >> 1)) ^ ((level + 1) ^ ((level + 1) >> 1));
        grayAdjacent = grayAdjacent && difference && (difference & (difference - 1)) == 0;
    }
    check(grayAdjacent, "adjacent physical luminance flips exactly one bit", passed);
    check(image.at<uint8_t>(575, 975) == 32, "unused payload cells are harmless zero symbols", passed);

    cv::Mat bgr, bgra;
    cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
    cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    check(fullFrame(codec.decode(bgr), frame), "BGR capture decode", passed);
    cv::Mat desktop(780, 1240, CV_8UC4, cv::Scalar(19, 26, 33, 255));
    bgra.copyTo(desktop(cv::Rect(117, 93, 1000, 600)));
    auto offset = codec.decode(desktop);
    check(fullFrame(offset, frame) && offset.canvas == cv::Rect(117, 93, 1000, 600),
          "offset BGRA canvas coordinates", passed);
    cv::Mat storage(620, 1020, CV_8UC1, cv::Scalar(11));
    auto noncontinuous = storage(cv::Rect(7, 9, 1000, 600));
    image.copyTo(noncontinuous);
    check(!noncontinuous.isContinuous() && fullFrame(codec.decode(noncontinuous), frame),
          "non-contiguous capture stride", passed);

    auto calibrated = image.clone();
    for (int y = 52; y < 576; ++y)
        for (int x = 24; x < 976; ++x) {
            bool inData = y >= 84;
            bool inPalette = y >= 52 && y < 76 && x < 256;
            if (inData || inPalette)
                calibrated.at<uint8_t>(y, x) = uint8_t(image.at<uint8_t>(y, x) * .7 + 25);
        }
    check(fullFrame(codec.decode(calibrated), frame), "observed reference mean calibrates gain and offset",
          passed);
    auto noisy = image.clone();
    for (int y = 84; y < 576; ++y)
        for (int x = 24; x < 976; ++x)
            noisy.at<uint8_t>(y, x) = uint8_t(int(noisy.at<uint8_t>(y, x)) + int(rng() % 15) - 7);
    check(fullFrame(codec.decode(noisy), frame), "two-by-two mean tolerates mild pixel noise", passed);

    auto corrected = image.clone();
    auto code = getCode(corrected, 17);
    for (unsigned i = 0; i < 24; ++i)
        code[i * 7 % 255] ^= uint8_t(1 + i * 3);
    putCode(corrected, 17, code);
    check(fullFrame(codec.decode(corrected), frame), "RS48 corrects 24 distinct byte errors", passed);
    auto damaged = image.clone();
    code = getCode(damaged, 17);
    for (unsigned i = 0; i < 25; ++i)
        code[i * 7 % 255] ^= uint8_t(0xa7 + i);
    putCode(damaged, 17, code);
    auto partial = codec.decode(damaged);
    check(partial.valid && partial.frame.packets.size() == Slots - 1,
          "uncorrectable block does not hide intact blocks", passed);
    bool absent = true;
    for (const auto &packet : partial.frame.packets)
        absent = absent && packet.index != frame.packets[17].index;
    check(absent, "uncorrectable block contributes no payload", passed);

    auto wrongCrc = image.clone();
    auto message = encodePacket(frame.packets[29]);
    message[20] ^= 0x95; // Leave the packet CRC wrong while generating a perfectly valid RS codeword.
    putCode(wrongCrc, 29, rs.encode(message));
    auto crcResult = codec.decode(wrongCrc);
    check(crcResult.valid && crcResult.frame.packets.size() == Slots - 1,
          "valid RS with invalid packet CRC rejected", passed);
    auto foreign = image.clone();
    auto foreignPacket = frame.packets[13];
    foreignPacket.transfer ^= 0x100000000ull;
    putCode(foreign, 13, rs.encode(encodePacket(foreignPacket)));
    check(codec.decode(foreign).frame.packets.size() == Slots - 1, "64-bit foreign transfer packet rejected",
          passed);

    WireFrame sparse;
    sparse.info = frame.info;
    for (unsigned i = 0; i < 9; ++i)
        sparse.packets[i] = frame.packets[Slots - 1 - i];
    auto sparseResult = codec.decode(codec.encode(sparse));
    bool sparseOk = sparseResult.valid && sparseResult.frame.packets.size() == 9;
    for (unsigned i = 0; i < 9 && sparseOk; ++i)
        sparseOk = samePacket(sparseResult.frame.packets[i], sparse.packets[i]);
    check(sparseOk, "partial real packets plus zero padding", passed);
    sparse.packets = {};
    auto empty = codec.decode(codec.encode(sparse));
    check(empty.valid && empty.frame.packets.empty() && empty.status == "payload_failed",
          "zero valid payload preserves header and canvas", passed);
    sparse = frame;
    sparse.info.complete = true;
    sparse.info.window = (uint64_t(1) << 53) + 17;
    auto done = codec.decode(codec.encode(sparse));
    check(fullFrame(done, sparse) && done.frame.info.complete, "done flag and window beyond 32 bits", passed);
    check(fullFrame(result, frame), "packet indices are independent of frame window and sequence", passed);

    auto oneBad = image.clone();
    auto header = readHeader(oneBad);
    header[0] ^= 1;
    writeHeader(oneBad, header, 0, false);
    check(fullFrame(codec.decode(oneBad), frame), "one invalid header copy tolerated", passed);
    auto allBad = image.clone();
    writeHeader(allBad, header, -1, false);
    check(!codec.decode(allBad).valid, "all header CRC copies invalid", passed);
    auto torn = image.clone();
    header = readHeader(torn);
    header[16] ^= 1;
    writeHeader(torn, header, 1, true);
    check(!codec.decode(torn).valid, "different CRC-valid header copies rejected", passed);
    for (int field : {0, 4, 5, 6, 7}) {
        auto invalid = image.clone();
        header = readHeader(invalid);
        if (field == 5)
            header[field] |= 2;
        else
            header[field] ^= 1;
        writeHeader(invalid, header);
        check(!codec.decode(invalid).valid, "unsupported header magic/version/flags/parity/cell rejected",
              passed);
    }
    auto zeroTransfer = image.clone();
    header = readHeader(zeroTransfer);
    std::fill(header.begin() + 8, header.begin() + 16, 0);
    writeHeader(zeroTransfer, header);
    check(!codec.decode(zeroTransfer).valid, "zero header transfer rejected", passed);
    auto collapsed = image.clone();
    collapsed(cv::Rect(24, 52, 232, 24)).setTo(96);
    auto paletteResult = codec.decode(collapsed);
    check(!paletteResult.valid && paletteResult.status == "palette_collapsed",
          "collapsed palette rejects payload", passed);
    cv::Mat scaled;
    cv::resize(image, scaled, cv::Size(960, 576), 0, 0, cv::INTER_LINEAR);
    check(!codec.decode(scaled).valid, "96 percent scaling rejected", passed);
    cv::resize(image, scaled, cv::Size(1100, 660), 0, 0, cv::INTER_NEAREST);
    check(!codec.decode(scaled).valid, "110 percent scaling rejected", passed);
    check(!codec.decode(cv::Mat()).valid, "empty image rejected", passed);
    check(!codec.decode(cv::Mat(12, 15, CV_8UC1, cv::Scalar(255))).valid, "tiny image rejected", passed);
    check(!codec.decode(cv::Mat(600, 1000, CV_32FC1, cv::Scalar(0))).valid, "float image rejected", passed);
    check(!codec.decode(cv::Mat(600, 1000, CV_8UC2, cv::Scalar(0, 0))).valid, "unsupported channels rejected",
          passed);
    bool throwZero = false;
    try {
        codec.encode(WireFrame{});
    } catch (const std::invalid_argument &) {
        throwZero = true;
    }
    check(throwZero, "sender rejects zero transfer header", passed);
    std::cout << "Frame codec self-test: " << passed << " checks passed\n";
}
} // namespace cst
