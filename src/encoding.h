/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include <cstdint>

namespace cst {
struct Encoding {
    unsigned width = 1000, height = 600, cell = 2, levels = 4, parity = 48, windowFrames = 256;
    void validate() const;
    unsigned bits() const;
    unsigned slots() const;
    unsigned messageBytes() const { return 255 - parity; }
    unsigned payloadBytes() const { return messageBytes() - 20; }
    unsigned manifestSlots() const { return (1496 + payloadBytes() - 1) / payloadBytes(); }
    unsigned windowChunks() const { return slots() * windowFrames; }
    uint64_t windowBytes() const { return uint64_t(windowChunks()) * payloadBytes(); }
    bool operator==(const Encoding &other) const;
    bool operator!=(const Encoding &other) const { return !(*this == other); }
    bool legacy() const { return *this == Encoding{}; }
};
constexpr unsigned MinCanvasWidth = 900, MinCanvasHeight = 300;
} // namespace cst
