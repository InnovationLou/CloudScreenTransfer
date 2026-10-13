/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "protocol.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cst {
namespace {
constexpr uint64_t MaxFileSize = uint64_t((std::numeric_limits<int64_t>::max)());
constexpr uint16_t Version = 1;
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void put16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
}
void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(value >> (8 * i));
}
void put64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(value >> (8 * i));
}
uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t get32(const uint8_t *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(p[i]) << (8 * i);
    return value;
}
uint64_t get64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t(p[i]) << (8 * i);
    return value;
}
char upper(char c) {
    return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
}
bool zeroMask(const FrameMask &mask) {
    return std::all_of(mask.begin(), mask.end(), [](uint8_t value) { return value == 0; });
}
void validSize(uint64_t size) {
    require(size <= MaxFileSize, "File exceeds signed 64-bit Windows offset range");
}
uint64_t ceilDiv(uint64_t value, uint64_t divisor) {
    return value / divisor + unsigned(value % divisor != 0);
}

// Win32 component rules: https://learn.microsoft.com/windows/win32/fileio/naming-a-file
void validFilename(const std::string &name) {
    require(!name.empty() && name.size() <= FilenameMaxUtf8Bytes, "Invalid UTF-8 filename byte length");
    unsigned units = 0;
    for (size_t position = 0; position < name.size();) {
        uint8_t first = uint8_t(name[position++]);
        uint32_t code = 0;
        unsigned following = 0;
        if (first < 0x80)
            code = first;
        else if (first >= 0xc2 && first <= 0xdf) {
            code = first & 31;
            following = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 15;
            following = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 7;
            following = 3;
        } else
            throw std::invalid_argument("Malformed UTF-8 filename");
        require(position + following <= name.size(), "Truncated UTF-8 filename");
        for (unsigned i = 0; i < following; ++i) {
            uint8_t next = uint8_t(name[position++]);
            require((next & 0xc0) == 0x80, "Malformed UTF-8 continuation");
            code = (code << 6) | (next & 63);
        }
        require((following != 1 || code >= 0x80) && (following != 2 || code >= 0x800) &&
                    (following != 3 || code >= 0x10000) && code <= 0x10ffff &&
                    !(code >= 0xd800 && code <= 0xdfff),
                "Noncanonical UTF-8 filename");
        require(code >= 32 &&
                    (code >= 128 || std::string("<>:\"/\\|?*").find(char(code)) == std::string::npos),
                "Filename is not one Windows component");
        units += code > 0xffff ? 2 : 1;
        require(units <= 255, "Filename exceeds Windows component length");
    }
    require(name.back() != '.' && name.back() != ' ', "Filename ends in a period or space");
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    for (char &c : stem)
        c = upper(c);
    require(stem != "CON" && stem != "PRN" && stem != "AUX" && stem != "NUL" && stem != "CONIN$" &&
                stem != "CONOUT$",
            "Reserved Windows filename");
    if (stem.size() >= 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
        auto digit = stem.substr(3);
        require(!(digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') && digit != "\xc2\xb9" &&
                    digit != "\xc2\xb2" && digit != "\xc2\xb3",
                "Reserved Windows serial/parallel filename");
    }
}
void validateFeedback(const Feedback &feedback) {
    char op = upper(feedback.operation);
    require(op == 'M' || op == 'P' || op == 'D', "Unknown feedback operation");
    if (op == 'M' || op == 'D') {
        require(feedback.window == 0 && zeroMask(feedback.mask),
                "Metadata/done feedback must have zero window and mask");
        if (op == 'D')
            require(feedback.transfer != 0, "Done feedback has no transfer ID");
    } else {
        require(feedback.transfer != 0 && feedback.window <= MaxFileSize,
                "Progress feedback transfer/window outside protocol range");
    }
}
std::string hex(uint64_t value, unsigned digits) {
    std::string out(digits, '0');
    for (unsigned i = 0; i < digits; ++i) {
        out[digits - i - 1] = "0123456789ABCDEF"[value & 15];
        value >>= 4;
    }
    return out;
}
bool unhex(const std::string &text, size_t at, size_t count, uint64_t &value) {
    value = 0;
    if (count > 16 || at > text.size() || count > text.size() - at)
        return false;
    for (size_t i = 0; i < count; ++i) {
        char c = upper(text[at + i]);
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = unsigned(c - '0');
        else if (c >= 'A' && c <= 'F')
            digit = unsigned(c - 'A' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    return true;
}
} // namespace

uint32_t crc32(const uint8_t *bytes, size_t count) {
    require(bytes || !count, "Null CRC input");
    uint32_t crc = ~0u;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
uint64_t transferId(const Manifest &manifest) {
    // Identity binds the displayed basename as well as content. This is a public
    // 64-bit identifier, not authentication; full SHA256 still verifies the file.
    uint64_t value = 14695981039346656037ull;
    auto append = [&](uint8_t byte) {
        value ^= byte;
        value *= 1099511628211ull;
    };
    manifest.encoding.validate();
    for (char c : std::string(manifest.encoding.legacy() ? "CST1ID" : "CST2ID"))
        append(uint8_t(c));
    if (!manifest.encoding.legacy()) {
        for (unsigned n :
             {manifest.encoding.width, manifest.encoding.height, manifest.encoding.cell,
              manifest.encoding.levels, manifest.encoding.parity, manifest.encoding.windowFrames}) {
            append(uint8_t(n));
            append(uint8_t(n >> 8));
        }
    }
    for (uint8_t byte : manifest.sha)
        append(byte);
    for (unsigned i = 0; i < 8; ++i)
        append(uint8_t(manifest.size >> (8 * i)));
    for (char c : manifest.filename)
        append(uint8_t(c));
    return value ? value : 1;
}
uint64_t chunkCount(uint64_t size) {
    validSize(size);
    return ceilDiv(size, PayloadBytes);
}
uint64_t windowCount(uint64_t size) {
    return ceilDiv(chunkCount(size), WindowChunks);
}
uint64_t dataOffset(uint64_t packetIndex) {
    require(packetIndex >= ManifestSlots && packetIndex - ManifestSlots <= MaxFileSize / PayloadBytes,
            "Packet offset outside signed 64-bit file range");
    return (packetIndex - ManifestSlots) * PayloadBytes;
}
uint32_t expectedPayload(uint64_t size, uint64_t packetIndex) {
    validSize(size);
    if (packetIndex < ManifestSlots)
        return PayloadBytes;
    uint64_t chunk = packetIndex - ManifestSlots;
    if (chunk >= chunkCount(size))
        return 0;
    return uint32_t((std::min)(uint64_t(PayloadBytes), size - chunk * PayloadBytes));
}
uint32_t requiredFrames(uint64_t size, uint64_t window) {
    uint64_t chunks = chunkCount(size), windows = ceilDiv(chunks, WindowChunks);
    if (window >= windows)
        return 0;
    uint64_t count = (std::min)(uint64_t(WindowChunks), chunks - window * WindowChunks);
    return uint32_t(ceilDiv(count, Slots));
}
bool maskGet(const FrameMask &mask, uint32_t frame) {
    require(frame < WindowFrames, "Feedback frame index out of range");
    return (mask[frame / 8] & (1u << (frame % 8))) != 0;
}
void maskSet(FrameMask &mask, uint32_t frame, bool value) {
    require(frame < WindowFrames, "Feedback frame index out of range");
    if (value)
        mask[frame / 8] |= uint8_t(1u << (frame % 8));
    else
        mask[frame / 8] &= uint8_t(~(1u << (frame % 8)));
}

ManifestWire encodeManifest(const Manifest &manifest) {
    validSize(manifest.size);
    validFilename(manifest.filename);
    ManifestWire out{};
    std::copy_n("CSTF", 4, out.begin());
    const auto &e = manifest.encoding;
    e.validate();
    put16(out.data() + 4, e.legacy() ? Version : 2);
    put16(out.data() + 6, ManifestHeaderBytes);
    const unsigned config[] = {ManifestBytes,     e.width,       e.height,         e.slots(),
                               CodeBytes,         e.parity,      e.messageBytes(), e.payloadBytes(),
                               e.manifestSlots(), e.windowFrames};
    for (size_t i = 0; i < 10; ++i)
        put16(out.data() + 8 + i * 2, uint16_t(config[i]));
    put64(out.data() + 28, manifest.size);
    put64(out.data() + 36, transferId(manifest));
    std::copy(manifest.sha.begin(), manifest.sha.end(), out.begin() + 44);
    put16(out.data() + 76, uint16_t(manifest.filename.size()));
    if (!e.legacy()) {
        out[78] = uint8_t(e.cell);
        out[79] = uint8_t(e.levels);
    }
    std::copy(manifest.filename.begin(), manifest.filename.end(), out.begin() + ManifestHeaderBytes);
    put32(out.data() + out.size() - 4, crc32(out.data(), out.size() - 4));
    return out;
}
Manifest decodeManifest(const ManifestWire &bytes) {
    const unsigned version = get16(bytes.data() + 4);
    require(std::equal(bytes.begin(), bytes.begin() + 4, "CSTF") && (version == Version || version == 2) &&
                get16(bytes.data() + 6) == ManifestHeaderBytes &&
                get32(bytes.data() + bytes.size() - 4) == crc32(bytes.data(), bytes.size() - 4),
            "Manifest magic/version/reserved/CRC failure");
    Manifest manifest;
    auto &e = manifest.encoding;
    if (version == 2) {
        e.width = get16(bytes.data() + 10);
        e.height = get16(bytes.data() + 12);
        e.parity = get16(bytes.data() + 18);
        e.windowFrames = get16(bytes.data() + 26);
        e.cell = bytes[78];
        e.levels = bytes[79];
        e.validate();
    } else
        require(get16(bytes.data() + 78) == 0, "Manifest reserved fields are not zero");
    const unsigned config[] = {ManifestBytes,     e.width,       e.height,         e.slots(),
                               CodeBytes,         e.parity,      e.messageBytes(), e.payloadBytes(),
                               e.manifestSlots(), e.windowFrames};
    for (size_t i = 0; i < 10; ++i)
        require(get16(bytes.data() + 8 + i * 2) == config[i], "Unsupported manifest transport configuration");
    manifest.size = get64(bytes.data() + 28);
    validSize(manifest.size);
    std::copy_n(bytes.begin() + 44, manifest.sha.size(), manifest.sha.begin());
    size_t length = get16(bytes.data() + 76);
    require(length > 0 && length <= FilenameMaxUtf8Bytes, "Manifest filename length invalid");
    manifest.filename.assign(reinterpret_cast<const char *>(bytes.data() + ManifestHeaderBytes), length);
    validFilename(manifest.filename);
    require(get64(bytes.data() + 36) == transferId(manifest),
            "Manifest transfer ID differs from full file identity");
    require(std::all_of(bytes.begin() + ManifestHeaderBytes + length, bytes.end() - 4,
                        [](uint8_t byte) { return byte == 0; }),
            "Manifest padding is not canonical zero");
    return manifest;
}
PacketWire encodePacket(const Packet &packet) {
    auto bytes = encodePacket(packet, Encoding{});
    PacketWire out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}
std::vector<uint8_t> encodePacket(const Packet &packet, const Encoding &encoding) {
    encoding.validate();
    require(packet.index < encoding.manifestSlots() + ceilDiv(MaxFileSize, encoding.payloadBytes()),
            "Packet index exceeds protocol file range");
    require(packet.data.size() == encoding.payloadBytes(), "Packet payload differs from encoding profile");
    if (!packet.transfer)
        require(packet.index == 0 &&
                    std::all_of(packet.data.begin(), packet.data.end(), [](uint8_t byte) { return !byte; }),
                "Only all-zero padding may have zero transfer ID");
    std::vector<uint8_t> out(encoding.messageBytes(), 0);
    put64(out.data(), packet.transfer);
    put64(out.data() + 8, packet.index);
    std::copy(packet.data.begin(), packet.data.end(), out.begin() + 16);
    put32(out.data() + out.size() - 4, crc32(out.data(), out.size() - 4));
    return out;
}
bool decodePacket(const PacketWire &bytes, Packet &packet) {
    return decodePacket(bytes.data(), bytes.size(), packet, Encoding{});
}
bool decodePacket(const uint8_t *bytes, size_t size, Packet &packet, const Encoding &encoding) {
    try {
        encoding.validate();
    } catch (const std::invalid_argument &) {
        return false;
    }
    if (!bytes || size != encoding.messageBytes() || get32(bytes + size - 4) != crc32(bytes, size - 4))
        return false;
    Packet candidate;
    candidate.transfer = get64(bytes);
    candidate.index = get64(bytes + 8);
    if (!candidate.transfer ||
        candidate.index >= encoding.manifestSlots() + ceilDiv(MaxFileSize, encoding.payloadBytes()))
        return false;
    candidate.data.assign(bytes + 16, bytes + size - 4);
    packet = candidate;
    return true;
}

std::string formatFeedback(const Feedback &feedback) {
    validateFeedback(feedback);
    std::string body = "CST1";
    body.push_back(upper(feedback.operation));
    body += hex(feedback.transfer, 16);
    body += hex(feedback.window, 16);
    for (uint8_t value : feedback.mask)
        body += hex(value, 2);
    body += hex(crc32(reinterpret_cast<const uint8_t *>(body.data()), body.size()), 8);
    require(body.size() == FeedbackBodyChars, "Internal feedback framing error");
    return body + '\r';
}
bool parseFeedback(const std::string &text, Feedback &feedback) {
    std::string body = text;
    if (!body.empty() && body.back() == '\n')
        body.pop_back();
    if (!body.empty() && body.back() == '\r')
        body.pop_back();
    if (body.size() != FeedbackBodyChars)
        return false;
    for (char &c : body) {
        if (uint8_t(c) >= 128)
            return false;
        c = upper(c);
    }
    if (body.compare(0, 4, "CST1"))
        return false;
    uint64_t checksum;
    if (!unhex(body, 101, 8, checksum) ||
        uint32_t(checksum) != crc32(reinterpret_cast<const uint8_t *>(body.data()), 101))
        return false;
    Feedback candidate;
    candidate.operation = body[4];
    if (!unhex(body, 5, 16, candidate.transfer) || !unhex(body, 21, 16, candidate.window))
        return false;
    for (size_t i = 0; i < candidate.mask.size(); ++i) {
        uint64_t value;
        if (!unhex(body, 37 + i * 2, 2, value))
            return false;
        candidate.mask[i] = uint8_t(value);
    }
    try {
        validateFeedback(candidate);
    } catch (const std::invalid_argument &) {
        return false;
    }
    feedback = candidate;
    return true;
}
void FeedbackParser::restartSuffix() {
    const std::string prefix = "CST1";
    for (size_t count = (std::min)(pending.size(), prefix.size()); count; --count)
        if (pending.compare(pending.size() - count, count, prefix, 0, count) == 0) {
            pending = pending.substr(pending.size() - count);
            return;
        }
    pending.clear();
}
std::optional<Feedback> FeedbackParser::push(char character) {
    if (character == '\r' || character == '\n') {
        Feedback feedback;
        bool valid = parseFeedback(pending, feedback);
        pending.clear();
        if (valid)
            return feedback;
        return std::nullopt;
    }
    if (uint8_t(character) >= 128) {
        pending.clear();
        return std::nullopt;
    }
    pending.push_back(upper(character));
    bool shape = pending.size() <= FeedbackBodyChars;
    if (pending.size() <= 4)
        shape = shape && std::string("CST1").compare(0, pending.size(), pending) == 0;
    else if (pending.size() == 5)
        shape = pending.back() == 'M' || pending.back() == 'P' || pending.back() == 'D';
    else
        shape = shape && ((pending.back() >= '0' && pending.back() <= '9') ||
                          (pending.back() >= 'A' && pending.back() <= 'F'));
    if (!shape)
        restartSuffix();
    return std::nullopt;
}
} // namespace cst
