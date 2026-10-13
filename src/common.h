/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace cst {
constexpr unsigned CanvasWidth = 1000, CanvasHeight = 600, Slots = 114;
constexpr unsigned CodeBytes = 255, ParityBytes = 48, MessageBytes = 207, PayloadBytes = 187;
constexpr unsigned ManifestSlots = 8, WindowFrames = 256, WindowChunks = Slots * WindowFrames;
constexpr uint64_t WindowBytes = uint64_t(WindowChunks) * PayloadBytes;
using Digest = std::array<uint8_t, 32>;
struct Packet {
    uint64_t transfer = 0, index = 0;
    std::array<uint8_t, PayloadBytes> data{};
};
struct FrameInfo {
    uint64_t transfer = 0, window = 0;
    uint32_t sequence = 0;
    bool complete = false;
};
struct WireFrame {
    FrameInfo info;
    std::array<Packet, Slots> packets{};
};
struct DecodedFrame {
    FrameInfo info;
    std::vector<Packet> packets;
};
struct Manifest {
    uint64_t size = 0;
    Digest sha{};
    std::string filename;
};
struct Feedback {
    char operation = 'M';
    uint64_t transfer = 0, window = 0;
    std::array<uint8_t, WindowFrames / 8> mask{};
};
struct Progress {
    uint64_t done = 0, total = 0, newBytes = 0;
    bool hashing = false, complete = false, resumed = false;
    std::wstring message, path;
};
using Cancel = std::function<bool()>;
using ProgressCallback = std::function<void(const Progress &)>;
} // namespace cst
