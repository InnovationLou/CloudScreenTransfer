/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include "common.h"
#include <cstddef>
#include <optional>

namespace cst {
constexpr size_t ManifestBytes = ManifestSlots * PayloadBytes;
constexpr size_t ManifestHeaderBytes = 80, FilenameMaxUtf8Bytes = 1020;
constexpr size_t FeedbackBodyChars = 109, FeedbackWireChars = FeedbackBodyChars + 1;
using ManifestWire = std::array<uint8_t, ManifestBytes>;
using PacketWire = std::array<uint8_t, MessageBytes>;
using FrameMask = std::array<uint8_t, WindowFrames / 8>;

uint32_t crc32(const uint8_t *bytes, size_t count);
uint64_t
transferId(const Manifest &manifest); // Stable FNV1a64 over domain + full SHA + LE64 size + exact UTF-8 name.
ManifestWire encodeManifest(const Manifest &manifest);      // Throws on invalid name/size.
Manifest decodeManifest(const ManifestWire &bytes);         // Throws on CRC/config/name/size failure.
PacketWire encodePacket(const Packet &packet);              // Packet{} is permitted padding.
bool decodePacket(const PacketWire &bytes, Packet &packet); // Rejects padding; output unchanged on failure.
std::vector<uint8_t> encodePacket(const Packet &packet, const Encoding &encoding);
bool decodePacket(const uint8_t *bytes, size_t size, Packet &packet, const Encoding &encoding);

// Counts apply to DATA ONLY. Metadata uses separate repeated announce frames.
uint64_t chunkCount(uint64_t size);
uint64_t windowCount(uint64_t size);
uint64_t dataOffset(uint64_t packetIndex); // index >= ManifestSlots, signed-Win32-file-range checked.
uint32_t expectedPayload(uint64_t size, uint64_t packetIndex); // Metadata=187; data outside file=0.
uint32_t requiredFrames(uint64_t size, uint64_t window);       // Empty or completed-prefix window=0 frames.
bool maskGet(const FrameMask &mask, uint32_t frame);
void maskSet(FrameMask &mask, uint32_t frame, bool value = true);

// Canonical uppercase CRC-protected ASCII; body=109 bytes, wire adds CR (110).
// M: window=0, zero mask; transfer may be 0. D: nonzero transfer, window=0, zero mask.
// P: nonzero transfer, data window may be the completed prefix; file layer checks actual range.
std::string formatFeedback(const Feedback &feedback);
bool parseFeedback(const std::string &text, Feedback &feedback);
class FeedbackParser {
    std::string pending;
    void restartSuffix();

  public:
    std::optional<Feedback> push(char character);
    void reset() { pending.clear(); }
};

void protocolSelfTest(); // Implemented in tests/protocol_tests.cpp; pure, no UI/file/audio calls.
} // namespace cst
