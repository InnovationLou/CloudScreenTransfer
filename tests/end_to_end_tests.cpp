/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "file_transfer.h"
#include "frame_codec.h"
#include "protocol.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cst {
namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(std::string("End-to-end self-test: ") + message);
}
std::wstring join(const std::wstring &directory, const std::wstring &name) {
    return directory + L"\\" + name;
}
std::wstring resolved(const std::wstring &name) {
    std::array<wchar_t, 32768> buffer{};
    DWORD n = GetFullPathNameW(name.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
    require(n && n < buffer.size(), "resolve test path");
    return std::wstring(buffer.data(), n);
}
bool within(const std::wstring &root, const std::wstring &name) {
    auto absolute = resolved(name);
    return _wcsicmp(absolute.c_str(), root.c_str()) == 0 ||
           (absolute.size() > root.size() && absolute[root.size()] == L'\\' &&
            _wcsnicmp(absolute.c_str(), root.c_str(), root.size()) == 0);
}
void removeOwnedTree(const std::wstring &root, const std::wstring &directory) {
    // Resolve each deletion target and keep it inside this atomically created
    // test tree. A reparse point is never followed or recursively removed.
    if (!within(root, directory))
        return;
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return;
    WIN32_FIND_DATAW item{};
    HANDLE find = FindFirstFileW(join(directory, L"*").c_str(), &item);
    if (find != INVALID_HANDLE_VALUE) {
        try {
            do {
                if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0)
                    continue;
                auto child = join(directory, item.cFileName);
                if ((item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !within(root, child))
                    continue;
                if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    removeOwnedTree(root, child);
                else
                    DeleteFileW(child.c_str());
            } while (FindNextFileW(find, &item));
        } catch (...) {
            FindClose(find);
            throw;
        }
        FindClose(find);
    }
    RemoveDirectoryW(directory.c_str());
}
struct TestTree {
    std::wstring root;
    TestTree() {
        std::array<wchar_t, 32768> buffer{};
        DWORD n = GetCurrentDirectoryW(DWORD(buffer.size()), buffer.data());
        require(n && n < buffer.size(), "read current build directory");
        auto parent = resolved(std::wstring(buffer.data(), n));
        while (parent.size() > 3 && parent.back() == L'\\')
            parent.pop_back();
        for (unsigned i = 0; i < 100; ++i) {
            auto candidate =
                resolved(join(parent, L"cst-e2e-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                          std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i)));
            require(within(parent, candidate) && candidate != parent,
                    "test directory must be beneath build directory");
            if (CreateDirectoryW(candidate.c_str(), nullptr)) {
                root = candidate;
                return;
            }
            require(GetLastError() == ERROR_ALREADY_EXISTS, "create unique test tree");
        }
        throw std::runtime_error("Cannot create unique end-to-end test tree");
    }
    ~TestTree() {
        try {
            if (!root.empty())
                removeOwnedTree(root, root);
        } catch (...) {
        }
    }
};
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit File(HANDLE value) : handle(value) {
        require(handle != INVALID_HANDLE_VALUE, "open fixture file");
    }
    ~File() { CloseHandle(handle); }
};
void directory(const std::wstring &name) {
    require(CreateDirectoryW(name.c_str(), nullptr) != 0, "create case directory");
}
std::vector<uint8_t> fixture(size_t size) {
    std::vector<uint8_t> bytes(size);
    uint32_t state = uint32_t(size) ^ 0xac763819u;
    for (auto &byte : bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = uint8_t(state);
    }
    return bytes; // Only the source fixture owns this pattern; no decoder knows it.
}
void writeFile(const std::wstring &name, const std::vector<uint8_t> &bytes) {
    File file(
        CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    for (size_t position = 0; position < bytes.size();) {
        DWORD count = 0, amount = DWORD(std::min<size_t>(65536, bytes.size() - position));
        require(WriteFile(file.handle, bytes.data() + position, amount, &count, nullptr) && count == amount,
                "write fixture bytes");
        position += count;
    }
}
std::vector<uint8_t> readFile(const std::wstring &name) {
    File file(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && size.QuadPart < 20 * 1024 * 1024,
            "bound output fixture size");
    std::vector<uint8_t> bytes(size_t(size.QuadPart));
    for (size_t position = 0; position < bytes.size();) {
        DWORD count = 0, amount = DWORD(std::min<size_t>(65536, bytes.size() - position));
        require(ReadFile(file.handle, bytes.data() + position, amount, &count, nullptr) && count == amount,
                "read actual output bytes");
        position += count;
    }
    return bytes;
}
bool hasData(const WireFrame &frame) {
    return std::any_of(frame.packets.begin(), frame.packets.end(),
                       [](const Packet &p) { return p.transfer && p.index >= ManifestSlots; });
}
void destroyBlock(cv::Mat &image, unsigned slot) {
    // Replace one entire interleaved RS block with a zero codeword. RS can
    // decode it, but the real packet transfer/CRC checks must reject it.
    for (unsigned byte = 0; byte < CodeBytes; ++byte)
        for (unsigned symbol = 0; symbol < 4; ++symbol) {
            size_t cell = (size_t(byte) * Slots + slot) * 4 + symbol;
            image(cv::Rect(24 + int(cell % 476) * 2, 84 + int(cell / 476) * 2, 2, 2)).setTo(32);
        }
}
struct Statistics {
    unsigned encoded = 0, decoded = 0, skipped = 0, headerLost = 0, damaged = 0, duplicated = 0, restarts = 0,
             feedback = 0;
    uint64_t newBytes = 0;
};
DecodeResult opticalRoundtrip(FrameCodec &codec, const WireFrame &wire, Statistics &stats,
                              bool damage = false) {
    auto image = codec.encode(wire);
    ++stats.encoded;
    if (damage) {
        destroyBlock(image, 0);
        ++stats.damaged;
    }
    auto decoded = codec.decode(image);
    ++stats.decoded;
    require(decoded.valid && decoded.canvas == cv::Rect(0, 0, 1000, 600),
            "real image header and native ROI decode");
    require(decoded.frame.info.transfer == wire.info.transfer &&
                decoded.frame.info.window == wire.info.window &&
                decoded.frame.info.sequence == wire.info.sequence &&
                decoded.frame.info.complete == wire.info.complete,
            "actual decoded frame identity");
    require(decoded.frame.packets.size() == Slots - unsigned(damage),
            "block CRC validation preserves remaining real packets");
    return decoded;
}
void deliverFeedback(Sender &sender, Receiver &receiver, uint64_t observedTransfer, Statistics &stats) {
    auto feedback = receiver.feedback();
    // The restarted receiver has no manifest. Bind M to the actual valid
    // optical header, exactly as GUI does; never take the ID from sender internals.
    if (feedback.operation == 'M' && !feedback.transfer)
        feedback.transfer = observedTransfer;
    auto ascii = formatFeedback(feedback);
    Feedback parsed;
    require(ascii.size() == FeedbackWireChars && parseFeedback(ascii, parsed) &&
                formatFeedback(parsed) == ascii,
            "real feedback passes canonical format and CRC parser");
    sender.feedback(parsed);
    ++stats.feedback;
}
void restartWithoutManifest(Sender &sender, std::unique_ptr<Receiver> &receiver, const std::wstring &output,
                            FrameCodec &codec, uint32_t &sequence, Statistics &stats,
                            const ProgressCallback &progress) {
    uint64_t saved = receiver->progress().done, newBefore = stats.newBytes;
    receiver->save();
    receiver.reset();
    receiver = std::make_unique<Receiver>(output, progress);
    require(receiver->feedback().operation == 'M' && receiver->progress().total == 0,
            "restarted receiver initially lacks manifest");
    auto orphan = sender.next(sequence++);
    require(std::all_of(orphan.packets.begin(), orphan.packets.end(),
                        [](const Packet &p) { return p.index >= ManifestSlots; }),
            "restart begins with a real data-only optical frame");
    auto first = opticalRoundtrip(codec, orphan, stats);
    receiver->ingest(first.frame);
    require(receiver->feedback().operation == 'M' && receiver->progress().total == 0 &&
                stats.newBytes == newBefore,
            "unknown data cannot initialize or contribute bytes before manifest");
    deliverFeedback(sender, *receiver, first.frame.info.transfer, stats);
    auto announcement = opticalRoundtrip(codec, sender.next(sequence++), stats);
    require(std::all_of(announcement.frame.packets.begin(), announcement.frame.packets.end(),
                        [](const Packet &p) { return p.index < ManifestSlots; }),
            "bound M causes optical metadata retransmission");
    receiver->ingest(announcement.frame);
    require(receiver->progress().resumed && receiver->progress().done == saved &&
                receiver->feedback().window == 1 && stats.newBytes == newBefore,
            "manifest restores exact durable prefix and partial bitmap without recounting bytes");
    deliverFeedback(sender, *receiver, announcement.frame.info.transfer, stats);
    ++stats.restarts;
}
void caseTransfer(const std::wstring &root, size_t size, bool impaired, bool restart) {
    auto start = Clock::now();
    auto caseRoot = join(root, L"size-" + std::to_wstring(size));
    directory(caseRoot);
    auto output = join(caseRoot, L"out");
    directory(output);
    auto source = join(caseRoot, L"\u771f\u5b9e\u95ed\u73af_" + std::to_wstring(size) + L".bin");
    auto expected = fixture(size);
    writeFile(source, expected);
    Statistics stats;
    ProgressCallback progress = [&](const Progress &p) { stats.newBytes += p.newBytes; };
    Sender sender(source);
    auto receiver = std::make_unique<Receiver>(output, progress);
    FrameCodec codec;
    uint32_t sequence = 0;
    bool restarted = false;
    for (unsigned attempt = 0; attempt < 5000 && !sender.complete(); ++attempt) {
        auto wire = sender.next(sequence++);
        bool data = hasData(wire);
        if (impaired && data && attempt % 17 == 0) {
            // A missed display update contributes no decoded header, packets or ACK.
            ++stats.skipped;
            continue;
        }
        auto image = codec.encode(wire);
        ++stats.encoded;
        if (impaired && data && attempt % 41 == 0) {
            for (unsigned copy = 0; copy < 3; ++copy)
                image(cv::Rect(24 + copy * 272, 28, 256, 16)).setTo(0);
            auto lost = codec.decode(image);
            ++stats.decoded;
            require(!lost.valid && lost.frame.packets.empty(),
                    "damaged header never authorizes payload or feedback");
            ++stats.headerLost;
            continue;
        }
        bool damage = data && (stats.damaged == 0 || (impaired && attempt % 11 == 0));
        if (damage) {
            destroyBlock(image, 0);
            ++stats.damaged;
        }
        auto decoded = codec.decode(image);
        ++stats.decoded;
        require(decoded.valid && decoded.frame.packets.size() == Slots - unsigned(damage),
                "real coded pixels recover full or partial packets");
        receiver->ingest(decoded.frame);
        if (data && (attempt % 13 == 0 || !impaired)) {
            auto duplicated = codec.decode(image);
            ++stats.decoded;
            uint64_t before = stats.newBytes;
            receiver->ingest(duplicated.frame);
            require(stats.newBytes == before, "duplicate optical frames contribute no new file bytes");
            ++stats.duplicated;
        }
        deliverFeedback(sender, *receiver, decoded.frame.info.transfer, stats);
        if (restart && !restarted && !receiver->complete() && receiver->feedback().window == 1 &&
            receiver->progress().done >= WindowBytes + 2 * uint64_t(Slots) * PayloadBytes) {
            restartWithoutManifest(sender, receiver, output, codec, sequence, stats, progress);
            restarted = true;
        }
    }
    require(sender.complete() && receiver->complete(),
            "both endpoints finish through actual optical/ASCII feedback loop");
    require(sender.progress().complete && receiver->progress().complete && sender.progress().done == size &&
                receiver->progress().done == size,
            "completed endpoint progress equals actual source length");
    require(stats.newBytes == size, "only unique actual payload bytes count across duplicates and restart");
    require(readFile(receiver->progress().path) == expected,
            "actual saved file exactly matches all source bytes");
    auto completed = opticalRoundtrip(codec, sender.next(sequence++), stats);
    require(completed.frame.info.complete, "verified final D is reflected in real optical done header");
    receiver->ingest(completed.frame);
    deliverFeedback(sender, *receiver, completed.frame.info.transfer, stats);
    require(!restart || (restarted && stats.restarts == 1),
            "large optical transfer really exercised receiver restart");
    if (impaired)
        require(stats.skipped && stats.headerLost && stats.damaged && stats.duplicated,
                "large case exercised all intended impairments");
    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Optical file case " << size << " B: encoded=" << stats.encoded
              << ", decoded=" << stats.decoded << ", skipped=" << stats.skipped
              << ", header_lost=" << stats.headerLost << ", damaged_blocks=" << stats.damaged
              << ", duplicate_frames=" << stats.duplicated << ", receiver_restarts=" << stats.restarts
              << ", elapsed_s=" << std::fixed << std::setprecision(3) << elapsed << '\n'
              << std::flush;
}
} // namespace

void endToEndSelfTest() {
    auto start = Clock::now();
    TestTree directory;
    for (size_t size : {size_t(0), size_t(1), size_t(187), size_t(188)})
        caseTransfer(directory.root, size, false, false);
    caseTransfer(directory.root, size_t(WindowBytes) + 600123, true, true);
    std::cout << "End-to-end optical file self-test: 5 cases passed, elapsed_s=" << std::fixed
              << std::setprecision(3) << std::chrono::duration<double>(Clock::now() - start).count()
              << " (offline; no display/capture/input).\n";
}
} // namespace cst
