/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "protocol.h"
extern "C" {
#include <correct.h>
}
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace cst {
namespace {
void check(bool value, const char *label) {
    if (!value)
        throw std::runtime_error(std::string("Protocol self-test: ") + label);
}
template <class Function> void rejects(Function function, const char *label) {
    bool failed = false;
    try {
        function();
    } catch (const std::invalid_argument &) {
        failed = true;
    }
    check(failed, label);
}
void reseal(ManifestWire &wire) {
    auto crc = crc32(wire.data(), wire.size() - 4);
    for (unsigned i = 0; i < 4; ++i)
        wire[wire.size() - 4 + i] = uint8_t(crc >> (8 * i));
}
std::string lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return text;
}
} // namespace

void protocolSelfTest() {
    check(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926u && crc32(nullptr, 0) == 0,
          "CRC32 standard vectors");
    rejects([] { crc32(nullptr, 1); }, "Null CRC buffer");
    Manifest manifest;
    manifest.filename = u8"示例-🧪.bin";
    for (size_t i = 0; i < manifest.sha.size(); ++i)
        manifest.sha[i] = uint8_t(i + 1);
    uint64_t stableId = transferId(manifest);
    check(stableId == 0x102f4044c5eb5459ull && transferId(manifest) == stableId,
          "Stable full-manifest ID golden vector");
    auto renamed = manifest;
    renamed.filename = "different-name.bin";
    auto resized = manifest;
    resized.size = 1;
    auto changedHash = manifest;
    changedHash.sha[31] ^= 1;
    check(transferId(renamed) != stableId && transferId(resized) != stableId &&
              transferId(changedHash) != stableId,
          "Identity includes filename, size, and all SHA bytes");
    Manifest emptyA, emptyB;
    emptyA.filename = "empty-a.txt";
    emptyB.filename = "empty-b.txt";
    check(transferId(emptyA) && transferId(emptyA) != transferId(emptyB),
          "Two empty basenames have different IDs");
    for (uint64_t size : {0ull, 1ull, 187ull, 188ull, (1ull << 32) + 17}) {
        manifest.size = size;
        auto wire = encodeManifest(manifest);
        auto decoded = decodeManifest(wire);
        check(decoded.size == size && decoded.filename == manifest.filename && decoded.sha == manifest.sha,
              "Manifest roundtrip");
    }
    const uint64_t maximum = uint64_t((std::numeric_limits<int64_t>::max)());
    manifest.size = maximum;
    check(decodeManifest(encodeManifest(manifest)).size == maximum, "INT64 max manifest");
    manifest.size = maximum + 1;
    rejects([&] { encodeManifest(manifest); }, "Unsigned size rejected above file API limit");
    manifest.size = 188;
    const std::string unsafe[] = {"",
                                  ".",
                                  "..",
                                  "../secret",
                                  "a/b",
                                  "a\\b",
                                  "C:secret",
                                  "x:stream",
                                  "a?b",
                                  "foo.",
                                  "foo ",
                                  "CON",
                                  "con.txt",
                                  "CON .txt",
                                  "PRN.bin",
                                  "AUX",
                                  "nul.tar.gz",
                                  "COM1",
                                  "lpt9.log",
                                  "COM\xc2\xb9.txt",
                                  "LPT\xc2\xb3",
                                  "CONIN$",
                                  "CONOUT$",
                                  std::string("a\0b", 3),
                                  "\xc0\xaf",
                                  "\xed\xa0\x80",
                                  "\xf0\x9f",
                                  "\xf4\x90\x80\x80",
                                  std::string(256, 'a'),
                                  std::string(FilenameMaxUtf8Bytes + 1, 'a')};
    for (const auto &name : unsafe) {
        auto bad = manifest;
        bad.filename = name;
        rejects([&] { encodeManifest(bad); }, "Unsafe/malformed filename rejected");
    }
    for (const auto &name : {std::string(".hidden"), std::string("COM0.txt"), std::string("COM10.txt"),
                             std::string("normal space.bin")}) {
        auto good = manifest;
        good.filename = name;
        check(decodeManifest(encodeManifest(good)).filename == name, "Valid Windows component");
    }
    auto wire = encodeManifest(manifest);
    wire[44] ^= 1;
    rejects([&] { decodeManifest(wire); }, "Manifest CRC rejects changed digest");
    wire = encodeManifest(manifest);
    wire[18] = 64;
    reseal(wire);
    rejects([&] { decodeManifest(wire); }, "Config mismatch with valid CRC");
    wire = encodeManifest(manifest);
    wire[36] ^= 1;
    reseal(wire);
    rejects([&] { decodeManifest(wire); }, "Transfer ID mismatch");
    wire = encodeManifest(manifest);
    wire[78] = 1;
    reseal(wire);
    rejects([&] { decodeManifest(wire); }, "Reserved metadata nonzero");
    wire = encodeManifest(manifest);
    wire[wire.size() - 5] = 1;
    reseal(wire);
    rejects([&] { decodeManifest(wire); }, "Canonical padding");
    wire = encodeManifest(manifest);
    wire[76] = 0;
    wire[77] = 0;
    reseal(wire);
    rejects([&] { decodeManifest(wire); }, "Empty wire filename");

    check(chunkCount(0) == 0 && windowCount(0) == 0 && requiredFrames(0, 0) == 0,
          "Empty file bypasses data windows");
    check(chunkCount(1) == 1 && chunkCount(187) == 1 && chunkCount(188) == 2, "Chunk edges");
    check(expectedPayload(0, 0) == 187 && expectedPayload(0, 7) == 187 && expectedPayload(0, 8) == 0,
          "Separate metadata phase");
    check(expectedPayload(1, 8) == 1 && expectedPayload(187, 8) == 187 && expectedPayload(188, 9) == 1 &&
              expectedPayload(188, 10) == 0,
          "Final chunk lengths");
    check(windowCount(WindowBytes) == 1 && windowCount(WindowBytes + 1) == 2 &&
              requiredFrames(WindowBytes, 0) == 256 && requiredFrames(WindowBytes + 1, 1) == 1 &&
              requiredFrames(WindowBytes + 1, 2) == 0,
          "Window edges");
    check(requiredFrames(114ull * 187, 0) == 1 && requiredFrames(114ull * 187 + 1, 0) == 2, "Frame edges");
    uint64_t packetIndex = ManifestSlots + (1ull << 32) / PayloadBytes + 2;
    check(dataOffset(packetIndex) > (1ull << 32) &&
              expectedPayload((1ull << 32) + 1000, packetIndex) == PayloadBytes,
          "Above 4GB data offset");
    uint64_t hugeWindow = (1ull << 32) + 1, hugeSize = hugeWindow * WindowBytes + 1;
    check(windowCount(hugeSize) == hugeWindow + 1 && requiredFrames(hugeSize, hugeWindow) == 1,
          "64bit window number");
    check(requiredFrames(1, (std::numeric_limits<uint64_t>::max)()) == 0,
          "No overflowing window multiplication");
    rejects([] { dataOffset(7); }, "Metadata is not file offset");
    rejects([] { dataOffset((std::numeric_limits<uint64_t>::max)()); }, "Offset multiplication overflow");
    rejects([] { chunkCount((std::numeric_limits<uint64_t>::max)()); }, "File length overflow");
    FrameMask mask{};
    maskSet(mask, 0);
    maskSet(mask, 255);
    check(maskGet(mask, 0) && maskGet(mask, 255) && !maskGet(mask, 1), "Mask bit order");
    maskSet(mask, 0, false);
    check(!maskGet(mask, 0), "Mask clear");
    rejects([&] { maskSet(mask, 256); }, "Mask bounds");

    Packet packet;
    packet.transfer = transferId(manifest);
    packet.index = packetIndex;
    // Independent input bytes are only compared AFTER actual decoding; the receiver never regenerates them.
    for (size_t i = 0; i < packet.data.size(); ++i)
        packet.data[i] = uint8_t(i * i * 29 + 37 * (i + 1));
    auto message = encodePacket(packet);
    Packet decoded;
    check(decodePacket(message, decoded) && decoded.transfer == packet.transfer &&
              decoded.index == packet.index && decoded.data == packet.data,
          "Unknown payload binary packet");
    auto changed = message;
    changed[16] ^= 1;
    check(!decodePacket(changed, decoded), "Payload CRC corruption");
    check(!decodePacket(encodePacket(Packet{}), decoded), "Padding never accepted");
    auto codec = std::unique_ptr<correct_reed_solomon, decltype(&correct_reed_solomon_destroy)>(
        correct_reed_solomon_create(correct_rs_primitive_polynomial_8_7_2_1_0, 1, 1, ParityBytes),
        &correct_reed_solomon_destroy);
    check(bool(codec), "RS codec allocation");
    std::array<uint8_t, CodeBytes> coded{};
    check(correct_reed_solomon_encode(codec.get(), message.data(), message.size(), coded.data()) >= 0,
          "RS encode");
    for (unsigned i = 0; i < ParityBytes / 2; ++i)
        coded[(i * 7) % CodeBytes] ^= uint8_t(0x81 + i);
    PacketWire corrected{};
    check(correct_reed_solomon_decode(codec.get(), coded.data(), coded.size(), corrected.data()) ==
                  MessageBytes &&
              decodePacket(corrected, decoded) && decoded.data == packet.data &&
              decoded.index == packet.index,
          "24 unknown-byte errors genuinely corrected by RS+CRC");

    Feedback feedback;
    feedback.operation = 'P';
    feedback.transfer = packet.transfer;
    feedback.window = hugeWindow;
    feedback.mask = mask;
    auto command = formatFeedback(feedback);
    check(command.size() == FeedbackWireChars && command.back() == '\r', "109 body plus CR");
    Feedback parsed;
    check(parseFeedback(command, parsed) && parsed.operation == 'P' && parsed.transfer == feedback.transfer &&
              parsed.window == hugeWindow && parsed.mask == mask && parseFeedback(lower(command), parsed),
          "Feedback case/CRC/64bit roundtrip");
    auto corrupt = command;
    corrupt[37] = corrupt[37] == 'A' ? 'B' : 'A';
    check(!parseFeedback(corrupt, parsed), "Feedback bit corruption");
    check(!parseFeedback(command.substr(0, command.size() - 3), parsed), "Truncated feedback");
    Feedback request;
    check(parseFeedback(formatFeedback(request), parsed) && !parsed.transfer, "Unknown-ID metadata request");
    request.transfer = packet.transfer;
    check(parseFeedback(formatFeedback(request), parsed), "Known-ID metadata request");
    request.operation = 'D';
    check(parseFeedback(formatFeedback(request), parsed) && parsed.operation == 'D',
          "SHA-verified done command");
    request.window = 1;
    rejects([&] { formatFeedback(request); }, "Done fields are fixed zero");
    request = Feedback{};
    request.operation = 'P';
    rejects([&] { formatFeedback(request); }, "Progress needs transfer ID");
    feedback.window = windowCount(maximum);
    check(parseFeedback(formatFeedback(feedback), parsed), "Completed-prefix progress accepted");
    ++feedback.window;
    rejects([&] { formatFeedback(feedback); }, "Feedback oversized window");
    FeedbackParser stream;
    unsigned recovered = 0;
    std::string incoming =
        "unrelated keys" + corrupt + command.substr(0, 51) + "broken\r" + lower(command) + command;
    for (char c : incoming)
        if (auto value = stream.push(c)) {
            ++recovered;
            check(value->transfer == packet.transfer && value->window == hugeWindow,
                  "Stream result identity");
        }
    check(recovered == 2, "Stream discards corruption/truncation and resynchronizes");
    stream.reset();
    for (char c : command.substr(0, command.size() - 1))
        check(!stream.push(c), "Stream waits for CR terminator");
    check(bool(stream.push('\r')), "Stream emits only complete command");
}
} // namespace cst
