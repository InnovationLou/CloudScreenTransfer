/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "file_transfer.h"
#include "protocol.h"
#include "frame_codec.h"
#include "image_runtime.h"
#include "settings.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>

namespace cst {
namespace {
void check(bool good, const char *message) {
    if (!good)
        throw std::runtime_error(std::string("File-transfer self-test: ") + message);
}
struct TestHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit TestHandle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~TestHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void close() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};
std::wstring path(const std::wstring &dir, const std::wstring &name) {
    return dir + L"\\" + name;
}
void directory(const std::wstring &name) {
    check(CreateDirectoryW(name.c_str(), nullptr) != 0, "create test directory");
}
void removeOwnedDirectory(const std::wstring &dir) noexcept {
    // This directory was created uniquely by this self-test. Never traverse a
    // reparse point, and never enumerate or remove its parent build directory.
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(path(dir, L"*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0)
                continue;
            std::wstring child = path(dir, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                removeOwnedDirectory(child);
            else
                DeleteFileW(child.c_str());
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}
struct TestDirectory {
    std::wstring root;
    TestDirectory() {
        std::array<wchar_t, 32768> current{};
        DWORD count = GetCurrentDirectoryW(DWORD(current.size()), current.data());
        check(count && count < current.size(), "read self-test working directory");
        std::wstring parent(current.data(), count);
        // Resolve the explicitly named target once before any eventual cleanup.
        std::array<wchar_t, 32768> resolved{};
        DWORD length = GetFullPathNameW(parent.c_str(), DWORD(resolved.size()), resolved.data(), nullptr);
        check(length && length < resolved.size(), "resolve self-test working directory");
        parent.assign(resolved.data(), length);
        while (!parent.empty() && parent.back() == L'\\')
            parent.pop_back();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            root = path(parent, L"cst-selftest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(root.c_str(), nullptr))
                return;
            check(GetLastError() == ERROR_ALREADY_EXISTS, "create unique self-test directory");
        }
        throw std::runtime_error("Cannot create unique self-test directory");
    }
    ~TestDirectory() { removeOwnedDirectory(root); }
};
std::vector<uint8_t> pattern(size_t size, uint32_t seed = 0x71a39105) {
    std::vector<uint8_t> result(size);
    for (auto &value : result) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        value = uint8_t(seed);
    }
    return result;
}
void writeFile(const std::wstring &name, const std::vector<uint8_t> &bytes) {
    TestHandle file(
        CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(file.value != INVALID_HANDLE_VALUE, "create test file");
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0, request = DWORD(std::min<size_t>(bytes.size() - offset, 65536));
        check(WriteFile(file.value, bytes.data() + offset, request, &count, nullptr) && count == request,
              "write test file");
        offset += count;
    }
}
std::vector<uint8_t> readFile(const std::wstring &name) {
    TestHandle file(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    check(file.value != INVALID_HANDLE_VALUE, "open test output");
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file.value, &size) && size.QuadPart >= 0 && size.QuadPart < 20 * 1024 * 1024,
          "bounded test output size");
    std::vector<uint8_t> bytes(size_t(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0, request = DWORD(std::min<size_t>(bytes.size() - offset, 65536));
        check(ReadFile(file.value, bytes.data() + offset, request, &count, nullptr) && count == request,
              "read test output");
        offset += count;
    }
    return bytes;
}
DecodedFrame decoded(const WireFrame &frame, bool losses = false, bool duplicates = false) {
    DecodedFrame out;
    out.info = frame.info;
    for (const Packet &p : frame.packets) {
        if (losses && p.index >= ManifestSlots && (p.index + frame.info.sequence * 7) % 13 == 0)
            continue;
        out.packets.push_back(p);
        if (duplicates && p.index % 11 == 0)
            out.packets.push_back(p);
    }
    std::reverse(out.packets.begin(), out.packets.end());
    return out;
}
DecodedFrame metadata(const WireFrame &frame) {
    DecodedFrame out;
    out.info = frame.info;
    for (unsigned i = 0; i < ManifestSlots; ++i)
        out.packets.push_back(frame.packets[i]);
    return out;
}
void drive(Sender &sender, Receiver &receiver, uint32_t &sequence, bool losses = false) {
    for (unsigned attempt = 0; attempt < 12000 && !sender.complete(); ++attempt) {
        WireFrame wire = sender.next(sequence++);
        receiver.ingest(decoded(wire, losses, true));
        sender.feedback(receiver.feedback());
    }
    check(sender.complete() && receiver.complete(),
          "transfer eventually completes under loss/reorder/duplicates");
    check(sender.next(sequence++).info.complete, "done frame follows verified D acknowledgement");
    Feedback roundtrip;
    check(parseFeedback(formatFeedback(receiver.feedback()), roundtrip) && roundtrip.operation == 'D',
          "canonical final D feedback");
}
void smallCases(const std::wstring &root) {
    const size_t sizes[] = {0, 1, 187, 188, 8193};
    for (unsigned number = 0; number < sizeof(sizes) / sizeof(sizes[0]); ++number) {
        std::wstring dir = path(root, L"small-" + std::to_wstring(number));
        directory(dir);
        std::wstring out = path(dir, L"out");
        directory(out);
        std::wstring source = path(dir, L"\u6d4b\u8bd5\u6570\u636e.bin");
        auto bytes = pattern(sizes[number], number + 1);
        writeFile(source, bytes);
        uint64_t hashMaximum = 0, totalNew = 0;
        Sender sender(source, [&](const Progress &p) {
            if (p.hashing)
                hashMaximum = std::max(hashMaximum, p.done);
        });
        Receiver receiver(out, [&](const Progress &p) { totalNew += p.newBytes; });
        check(hashMaximum == sizes[number], "streaming source hash progress reaches actual size");
        uint32_t sequence = 0;
        drive(sender, receiver, sequence, true);
        check(readFile(receiver.progress().path) == bytes, "exact bytes for boundary-size/Unicode file");
        check(totalNew == bytes.size(), "duplicate packets never add to new-byte count");
        check(receiver.progress().done == bytes.size(), "completed progress bytes match file size");
        check(windowCount(sender.manifest().size) == (sizes[number] ? 1u : 0u),
              "zero-byte file has no data windows");
    }
}
void resumeCase(const std::wstring &root) {
    std::wstring dir = path(root, L"resume");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    auto bytes = pattern(size_t(WindowBytes) + 500000);
    std::wstring source = path(dir, L"multiwindow.bin");
    writeFile(source, bytes);
    Sender sender(source);
    auto receiver = std::make_unique<Receiver>(output);
    uint32_t sequence = 0;
    receiver->ingest(metadata(sender.next(sequence++)));
    sender.feedback(receiver->feedback());
    for (unsigned f = 0; f < WindowFrames + 9; ++f) {
        receiver->ingest(decoded(sender.next(sequence++)));
        sender.feedback(receiver->feedback());
    }
    check(!receiver->complete() && receiver->feedback().window == 1 &&
              receiver->progress().done > WindowBytes,
          "partial second window before restart");
    uint64_t saved = receiver->progress().done;
    receiver.reset();
    // A fresh process asks for metadata before it opens and validates the checkpoint.
    receiver = std::make_unique<Receiver>(output);
    check(receiver->feedback().operation == 'M', "resume requires incoming manifest");
    sender.feedback(receiver->feedback());
    receiver->ingest(metadata(sender.next(sequence++)));
    check(receiver->progress().resumed && receiver->progress().done == saved &&
              receiver->feedback().window == 1,
          "durable prefix and partial bitmap resume");
    Feedback resumed = receiver->feedback();
    check(maskGet(resumed.mask, 0), "resumed bitmap acknowledges saved data frame");
    sender.feedback(resumed);
    drive(sender, *receiver, sequence, true);
    check(readFile(receiver->progress().path) == bytes, "resumed multiwindow output exact");
    std::wstring completedPath = receiver->progress().path;
    receiver.reset();
    receiver = std::make_unique<Receiver>(output);
    receiver->ingest(metadata(sender.next(sequence++)));
    check(receiver->complete() && receiver->progress().resumed && receiver->progress().path == completedPath,
          "already-completed resume hashes actual output");
    check(receiver->feedback().operation == 'D', "already-completed resume emits D only after hash");
    receiver.reset();
    // Modify the final file; the persisted completion marker must not authorize D.
    TestHandle changed(CreateFileW(completedPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
    check(changed.value != INVALID_HANDLE_VALUE, "open completed output for mutation test");
    uint8_t wrong = uint8_t(bytes[0] ^ 0x80);
    DWORD written = 0;
    check(WriteFile(changed.value, &wrong, 1, &written, nullptr) && written == 1, "mutate completed output");
    changed.close();
    Receiver invalid(output);
    bool rejected = false;
    try {
        invalid.ingest(metadata(sender.next(sequence++)));
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected && !invalid.complete() && invalid.feedback().operation != 'D',
          "completed resume rejects changed output SHA");
}
void freshDestinationCase(const std::wstring &root) {
    std::wstring dir = path(root, L"fresh-destination");
    directory(dir);
    std::wstring originalOutput = path(dir, L"original");
    directory(originalOutput);
    std::wstring newOutput = path(dir, L"new");
    directory(newOutput);
    auto bytes = pattern(size_t(WindowBytes) + 500000, 0x2710c3);
    std::wstring source = path(dir, L"new-destination.bin");
    writeFile(source, bytes);
    Sender sender(source);
    uint32_t sequence = 0;
    {
        Receiver original(originalOutput);
        original.ingest(metadata(sender.next(sequence++)));
        sender.feedback(original.feedback());
        for (unsigned f = 0; f < WindowFrames + 2; ++f) {
            original.ingest(decoded(sender.next(sequence++)));
            sender.feedback(original.feedback());
        }
        check(original.feedback().window == 1 && sender.next(sequence++).info.window == 1 &&
                  !original.complete(),
              "sender advances to window 1 before new destination");
        Feedback stale;
        stale.operation = 'P';
        stale.transfer = sender.id();
        stale.window = 0;
        sender.feedback(stale);
        check(sender.next(sequence++).info.window == 1,
              "normal active sender ignores stale window-0 feedback");
    }
    Receiver fresh(newOutput);
    Feedback request = fresh.feedback();
    request.transfer = sender.id();
    sender.feedback(request);
    fresh.ingest(metadata(sender.next(sequence++)));
    check(!fresh.progress().resumed && fresh.feedback().window == 0 && fresh.progress().done == 0,
          "new empty directory requests window 0");
    sender.feedback(fresh.feedback());
    check(sender.next(sequence++).info.window == 0,
          "metadata resynchronization permits authoritative rewind to window 0");
    drive(sender, fresh, sequence, true);
    check(readFile(fresh.progress().path) == bytes, "rewound fresh destination receives entire file exactly");
}
void collisionCase(const std::wstring &root) {
    std::wstring dir = path(root, L"collision");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    std::wstring source = path(dir, L"collision.bin");
    auto bytes = pattern(1001);
    writeFile(source, bytes);
    auto original = pattern(12, 9);
    std::wstring existing = path(output, L"collision.bin");
    writeFile(existing, original);
    Sender sender(source);
    Receiver receiver(output);
    uint32_t sequence = 0;
    drive(sender, receiver, sequence);
    check(receiver.progress().path != existing && readFile(existing) == original,
          "existing output preserved on filename collision");
    check(readFile(receiver.progress().path) == bytes, "unique collision output exact");
}
void sameContentNamesCase(const std::wstring &root) {
    std::wstring dir = path(root, L"same-content");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    for (size_t size : {size_t(0), size_t(513)}) {
        std::wstring firstPath;
        uint64_t firstId = 0;
        auto bytes = pattern(size);
        for (unsigned n = 0; n < 2; ++n) {
            std::wstring name = L"same-" + std::to_wstring(size) + L"-" + std::to_wstring(n) + L".bin";
            std::wstring source = path(dir, name);
            writeFile(source, bytes);
            Sender sender(source);
            Receiver receiver(output);
            uint32_t sequence = 0;
            drive(sender, receiver, sequence);
            check(receiver.progress().path == path(output, name), "same-content file keeps its own basename");
            check(readFile(receiver.progress().path) == bytes, "same-content file exact bytes");
            if (!n) {
                firstPath = receiver.progress().path;
                firstId = sender.id();
            } else
                check(sender.id() != firstId && receiver.progress().path != firstPath,
                      "different basenames have distinct wire IDs and checkpoints");
        }
    }
}
void invalidMetadataCase(const std::wstring &root) {
    std::wstring dir = path(root, L"metadata");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    auto bytes = pattern(389);
    std::wstring source = path(dir, L"metadata.bin");
    writeFile(source, bytes);
    Sender sender(source);
    Receiver receiver(output);
    uint32_t sequence = 0;
    DecodedFrame bad = metadata(sender.next(sequence++));
    bad.packets[7].data[PayloadBytes - 1] ^= 1;
    receiver.ingest(bad);
    check(receiver.feedback().operation == 'M' && receiver.progress().total == 0 && !receiver.complete(),
          "bad full-manifest CRC cannot initialize transfer");
    drive(sender, receiver, sequence);
    check(readFile(receiver.progress().path) == bytes, "metadata retry recovers");
    // A manifest with a recomputed CRC still cannot carry a path traversal name.
    Manifest unsafe = sender.manifest();
    unsafe.filename = "../escaped.bin";
    bool rejected = false;
    try {
        (void)encodeManifest(unsafe);
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "unsafe filename rejected before transmission");
}
void sourceLockAndCancelCase(const std::wstring &root) {
    std::wstring dir = path(root, L"locked");
    directory(dir);
    std::wstring source = path(dir, L"locked.bin");
    writeFile(source, pattern(200000));
    {
        Sender sender(source);
        TestHandle writer(CreateFileW(source.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, 0, nullptr));
        check(writer.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
              "source stays locked against writes");
        check(!DeleteFileW(source.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
              "source stays locked against deletion");
    }
    {
        TestHandle existingWriter(CreateFileW(source.c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                              nullptr));
        check(existingWriter.value != INVALID_HANDLE_VALUE, "open external writer before sender");
        bool rejected = false;
        try {
            Sender sender(source);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "source already writable by another handle is rejected");
    }
    unsigned checks = 0;
    bool cancelled = false;
    try {
        Sender sender(source, {}, [&] { return ++checks > 2; });
    } catch (const std::exception &) {
        cancelled = true;
    }
    check(cancelled, "source hash cancellation interrupts streaming read");
    TestHandle writer(CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    check(writer.value != INVALID_HANDLE_VALUE, "cancelled source hash releases file and crypto resources");
}
void changedSourceCase(const std::wstring &root) {
    std::wstring dir = path(root, L"changed-source");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    std::wstring source = path(dir, L"changed.bin");
    auto bytes = pattern(50000);
    writeFile(source, bytes);
    uint64_t originalId = 0;
    {
        Sender sender(source);
        Receiver receiver(output);
        uint32_t sequence = 0;
        originalId = sender.id();
        receiver.ingest(metadata(sender.next(sequence++)));
        sender.feedback(receiver.feedback());
        receiver.ingest(decoded(sender.next(sequence++)));
        check(receiver.progress().done > 0 && !receiver.complete(),
              "old source version leaves partial checkpoint");
    }
    bytes[0] ^= 0xa7;
    {
        TestHandle writer(CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
        check(writer.value != INVALID_HANDLE_VALUE, "modify source between transfer sessions");
        DWORD count = 0;
        check(WriteFile(writer.value, bytes.data(), 1, &count, nullptr) && count == 1, "change source bytes");
    }
    Sender sender(source);
    Receiver receiver(output);
    uint32_t sequence = 0;
    check(sender.id() != originalId, "changed source receives a distinct stable file identity");
    receiver.ingest(metadata(sender.next(sequence++)));
    check(!receiver.progress().resumed && receiver.progress().done == 0,
          "changed source cannot reuse stale partial checkpoint");
    sender.feedback(receiver.feedback());
    drive(sender, receiver, sequence);
    check(readFile(receiver.progress().path) == bytes, "changed source output exact");
}
void receiveCancelCase(const std::wstring &root) {
    std::wstring dir = path(root, L"cancel");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    auto bytes = pattern(100000);
    std::wstring source = path(dir, L"cancel.bin");
    writeFile(source, bytes);
    Sender sender(source);
    bool stop = false;
    uint32_t sequence = 0;
    {
        Receiver receiver(output, {}, [&] { return stop; });
        receiver.ingest(metadata(sender.next(sequence++)));
        sender.feedback(receiver.feedback());
        receiver.ingest(decoded(sender.next(sequence++)));
        sender.feedback(receiver.feedback());
        stop = true;
        receiver.save(); // Must commit accepted bytes despite the active Cancel flag.
        bool interrupted = false;
        try {
            receiver.ingest(decoded(sender.next(sequence++)));
        } catch (const std::exception &) {
            interrupted = true;
        }
        check(interrupted && !receiver.complete(), "cancelled receiver cannot emit completion");
    }
    stop = false;
    Receiver receiver(output);
    sender.feedback(receiver.feedback());
    receiver.ingest(metadata(sender.next(sequence++)));
    check(receiver.progress().resumed && receiver.progress().done > 0,
          "cancel retains reusable partial checkpoint");
    sender.feedback(receiver.feedback());
    drive(sender, receiver, sequence);
    check(readFile(receiver.progress().path) == bytes, "cancelled receive resumes exactly");
}
void hashMismatchCase(const std::wstring &root) {
    std::wstring dir = path(root, L"hash-mismatch");
    directory(dir);
    std::wstring output = path(dir, L"out");
    directory(output);
    auto bytes = pattern(200);
    std::wstring source = path(dir, L"bad-hash.bin");
    writeFile(source, bytes);
    Sender sender(source);
    Manifest lie = sender.manifest();
    lie.sha[15] ^= 1;
    ManifestWire encoded = encodeManifest(lie);
    uint64_t id = transferId(lie);
    DecodedFrame metadataFrame;
    metadataFrame.info.transfer = id;
    for (unsigned i = 0; i < ManifestSlots; ++i) {
        Packet p;
        p.transfer = id;
        p.index = i;
        std::copy_n(encoded.begin() + i * PayloadBytes, PayloadBytes, p.data.begin());
        metadataFrame.packets.push_back(p);
    }
    Receiver receiver(output);
    receiver.ingest(metadataFrame);
    DecodedFrame data;
    data.info.transfer = id;
    for (unsigned i = 0; i < 2; ++i) {
        Packet p;
        p.transfer = id;
        p.index = ManifestSlots + i;
        unsigned n = unsigned(std::min<size_t>(PayloadBytes, bytes.size() - i * PayloadBytes));
        std::copy_n(bytes.begin() + i * PayloadBytes, n, p.data.begin());
        data.packets.push_back(p);
    }
    bool rejected = false;
    try {
        receiver.ingest(data);
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected && !receiver.complete() && receiver.feedback().operation != 'D',
          "full SHA mismatch never completes");
    check(GetFileAttributesW(path(output, L"bad-hash.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
          "SHA mismatch never publishes final output");
}
void highOffsetCase(const std::wstring &root) {
    std::wstring name = path(root, L"sparse-offset.bin");
    TestHandle file(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    check(file.value != INVALID_HANDLE_VALUE, "create sparse offset test file");
    DWORD returned = 0;
    check(DeviceIoControl(file.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0,
          "mark test file sparse (NTFS)");
    const uint64_t offset = (uint64_t(1) << 32) + 12345;
    LARGE_INTEGER where{};
    where.QuadPart = LONGLONG(offset);
    check(SetFilePointerEx(file.value, where, nullptr, FILE_BEGIN) != 0, "seek above 4 GiB");
    const std::array<uint8_t, 4> expected{{0x19, 0x72, 0xf4, 0x81}};
    DWORD count = 0;
    check(WriteFile(file.value, expected.data(), DWORD(expected.size()), &count, nullptr) &&
              count == expected.size(),
          "write above 4 GiB");
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file.value, &size) && uint64_t(size.QuadPart) == offset + expected.size(),
          "64-bit file size preserved");
    check(SetFilePointerEx(file.value, where, nullptr, FILE_BEGIN) != 0, "read seek above 4 GiB");
    std::array<uint8_t, 4> actual{};
    check(ReadFile(file.value, actual.data(), DWORD(actual.size()), &count, nullptr) &&
              count == actual.size() && actual == expected,
          "64-bit sparse offset read exact");
    uint64_t packet = ManifestSlots + offset / PayloadBytes;
    check(dataOffset(packet) > 0xffffffffULL &&
              expectedPayload(offset + PayloadBytes, packet) == PayloadBytes,
          "protocol retains high 64-bit packet offsets");

    // An intentionally synthetic, CRC-valid resumed prefix lets the real receiver
    // write above 4 GiB without transmitting or hashing that prefix. This fixture
    // tests offset handling; it is never treated as a completed/hash-verified file.
    std::wstring output = path(root, L"high-offset-receiver");
    directory(output);
    Manifest manifest;
    manifest.filename = "above4gib.bin";
    for (unsigned i = 0; i < manifest.sha.size(); ++i)
        manifest.sha[i] = uint8_t(i + 1);
    const uint64_t resumedWindow = 800, prefix = resumedWindow * WindowBytes;
    manifest.size = prefix + 2 * WindowBytes;
    uint64_t id = transferId(manifest);
    std::wostringstream stem;
    stem << L".cst-" << std::hex << std::setfill(L'0') << std::setw(16) << id << L"-" << std::setw(8)
         << crc32(reinterpret_cast<const uint8_t *>(manifest.filename.data()), manifest.filename.size());
    std::wstring partPath = path(output, stem.str() + L".part");
    TestHandle partial(CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
    check(partial.value != INVALID_HANDLE_VALUE, "create sparse resumed-prefix fixture");
    check(DeviceIoControl(partial.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0,
          "mark resumed-prefix fixture sparse");
    where.QuadPart = LONGLONG(prefix);
    check(SetFilePointerEx(partial.value, where, nullptr, FILE_BEGIN) && SetEndOfFile(partial.value),
          "extend sparse fixture past 4 GiB");
    partial.close();
    std::vector<uint8_t> state{'C', 'S', 'T', 'S', 'T', 'A', 'T', 'E'};
    auto put = [&](uint64_t value, unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            state.push_back(uint8_t(value >> (8 * i)));
    };
    put(1, 4);
    put(0, 4);
    put(id, 8);
    put(manifest.size, 8);
    state.insert(state.end(), manifest.sha.begin(), manifest.sha.end());
    put(resumedWindow, 8);
    state.insert(state.end(), WindowChunks / 8, 0);
    put(manifest.filename.size(), 4);
    put(0, 4);
    state.insert(state.end(), manifest.filename.begin(), manifest.filename.end());
    put(crc32(state.data(), state.size()), 4);
    writeFile(path(output, stem.str() + L".state"), state);
    const uint64_t targetOffset = prefix + 5 * PayloadBytes;
    auto chunkBytes = pattern(PayloadBytes, 0x8942);
    {
        Receiver receiver(output);
        ManifestWire wire = encodeManifest(manifest);
        DecodedFrame announce;
        announce.info.transfer = id;
        for (unsigned i = 0; i < ManifestSlots; ++i) {
            Packet p;
            p.transfer = id;
            p.index = i;
            std::copy_n(wire.begin() + i * PayloadBytes, PayloadBytes, p.data.begin());
            announce.packets.push_back(p);
        }
        receiver.ingest(announce);
        check(receiver.progress().resumed && receiver.feedback().window == resumedWindow,
              "receiver accepts bounded high-offset checkpoint fixture");
        Packet p;
        p.transfer = id;
        p.index = ManifestSlots + resumedWindow * WindowChunks + 5;
        std::copy(chunkBytes.begin(), chunkBytes.end(), p.data.begin());
        DecodedFrame data;
        data.info.transfer = id;
        data.info.window = resumedWindow;
        data.packets.push_back(p);
        receiver.ingest(data);
        check(receiver.progress().done == prefix + PayloadBytes &&
                  receiver.progress().total == manifest.size && !receiver.complete(),
              "receiver progress uses true 64-bit prefix plus unique chunk");
    }
    TestHandle result(CreateFileW(partPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    check(result.value != INVALID_HANDLE_VALUE, "open high-offset receiver output");
    where.QuadPart = LONGLONG(targetOffset);
    check(SetFilePointerEx(result.value, where, nullptr, FILE_BEGIN) != 0,
          "seek actual receiver chunk above 4 GiB");
    std::vector<uint8_t> actualChunk(PayloadBytes);
    check(ReadFile(result.value, actualChunk.data(), PayloadBytes, &count, nullptr) &&
              count == PayloadBytes && actualChunk == chunkBytes,
          "actual receiver writes correct bytes beyond 4 GiB");
}
void advancedCases(const std::wstring &root) {
    const Encoding profiles[] = {{900, 500, 3, 8, 64, 16},    {900, 400, 4, 2, 16, 16},
                                 {1024, 620, 5, 4, 24, 32},   {1000, 600, 2, 16, 32, 64},
                                 {1280, 720, 2, 8, 48, 64},   {1000, 600, 8, 16, 64, 16},
                                 {1920, 1080, 2, 16, 64, 16}, {900, 300, 2, 2, 64, 16}};
    for (unsigned i = 0; i < std::size(profiles); ++i) {
        const auto e = profiles[i];
        e.validate();
        auto dir = path(root, L"advanced-" + std::to_wstring(i));
        directory(dir);
        auto out = path(dir, L"out");
        directory(out);
        auto source = path(dir, L"编码参数.bin");
        auto bytes = pattern(i < 2 ? size_t(e.windowBytes()) + e.payloadBytes() + 3 : 70001, 0x182abf + i);
        writeFile(source, bytes);
        Sender sender(source, {}, {}, e);
        uint64_t fresh = 0;
        auto makeReceiver = [&] {
            return std::make_unique<Receiver>(out, [&](const Progress &p) { fresh += p.newBytes; });
        };
        auto receiver = makeReceiver();
        FrameCodec codec;
        bool resumed = false;
        for (unsigned frame = 0; frame < 2500 && !sender.complete(); ++frame) {
            auto wire = sender.next(frame);
            auto pixels = codec.encode(wire);
            cv::Mat bgra;
            grayToBgra(pixels, bgra);
            // Exercise the actual BGRA capture decoder, with offset/strided storage.
            cv::Mat desktop(int(e.height) + 23, int(e.width) + 39, CV_8UC4, cv::Scalar(7, 11, 19, 255));
            bgra.copyTo(desktop(cv::Rect(17, 9, int(e.width), int(e.height))));
            auto decoded = codec.decode(desktop);
            check(decoded.valid && decoded.canvas == cv::Rect(17, 9, int(e.width), int(e.height)) &&
                      decoded.frame.info.encoding == e,
                  "header automatically detects custom encoding");
            if (frame % 9 != 7)
                receiver->ingest(decoded.frame);
            Feedback reply;
            check(parseFeedback(formatFeedback(receiver->feedback()), reply),
                  "custom profile keyboard feedback roundtrip");
            sender.feedback(reply);
            if (i == 0 && !resumed && !receiver->complete() && receiver->progress().done > bytes.size() / 3) {
                receiver->save();
                receiver.reset();
                receiver = makeReceiver();
                resumed = true;
            }
        }
        check(sender.complete() && receiver->complete(),
              "custom encoding completes through image/CRC/file/SHA");
        check(fresh == bytes.size() && readFile(receiver->progress().path) == bytes,
              "custom profile resume preserves exact unique bytes");
        check(i != 0 || resumed, "custom encoding really exercised checkpoint restart");
    }
    Manifest a;
    a.filename = "same.bin";
    a.size = 123;
    auto b = a;
    b.encoding = profiles[0];
    check(transferId(a) != transferId(b), "encoding layouts have independent checkpoint identities");
    auto bad = profiles[0];
    bad.levels = 3;
    bool rejected = false;
    try {
        bad.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "unsupported gray count rejected");
    bad = {900, 300, 8, 2, 64, 16};
    rejected = false;
    try {
        bad.validate();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "insufficient optical capacity rejected before transfer");
    std::cout
        << "Advanced encoding: 8 optical file profiles, loss, checkpoint restart and exact SHA passed.\n";
}
void settingsCase(const std::wstring &root) {
    auto name = path(root, L"用户参数.ini");
    std::wstring warning;
    auto defaults = loadSettings(name, warning);
    check(warning.empty() && defaults.encoding == Encoding{} && defaults.clickCanvas &&
              defaults.feedbackEnabled,
          "missing settings preserve working recommended defaults");
    Settings s;
    s.encoding = {900, 500, 3, 8, 64, 64};
    s.send = true;
    s.sendFps = 8;
    s.captureFps = 31;
    s.progressAckMs = 5555;
    s.controlAckMs = 1600;
    s.keyHoldMs = 17;
    s.keyGapMs = 23;
    s.imageFreshMs = 1400;
    s.keyMode = FeedbackKeyMode::Unicode;
    s.clickCanvas = false;
    s.restoreMouse = true;
    s.feedbackEnabled = false;
    s.source = L"C:\\中文目录\\测试文件.bin";
    s.directory = L"D:\\保存目录 空格";
    s.process = L"AnotherVDI.exe";
    s.windowSaved = true;
    s.windowX = -1500;
    s.windowY = 70;
    s.windowWidth = 1040;
    s.windowHeight = 720;
    saveSettings(name, s);
    auto actual = loadSettings(name, warning);
    check(warning.empty() && actual.encoding == s.encoding && actual.send == s.send &&
              actual.source == s.source && actual.directory == s.directory && actual.process == s.process &&
              actual.sendFps == s.sendFps && actual.captureFps == s.captureFps &&
              actual.progressAckMs == s.progressAckMs && actual.controlAckMs == s.controlAckMs &&
              actual.keyHoldMs == s.keyHoldMs && actual.keyGapMs == s.keyGapMs &&
              actual.imageFreshMs == s.imageFreshMs && actual.keyMode == s.keyMode &&
              actual.clickCanvas == s.clickCanvas && actual.restoreMouse == s.restoreMouse &&
              actual.feedbackEnabled == s.feedbackEnabled && actual.windowSaved &&
              actual.windowX == s.windowX && actual.windowY == s.windowY &&
              actual.windowWidth == s.windowWidth && actual.windowHeight == s.windowHeight,
          "all encoding/timing/Unicode paths/main options survive disk reload");
    s.sendFps = 12;
    s.encoding.levels = 16;
    saveSettings(name, s);
    actual = loadSettings(name, warning);
    check(actual.sendFps == 12 && actual.encoding.levels == 16,
          "atomic replacement invalidates cached INI values");
    {
        TestHandle held(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        check(held.value != INVALID_HANDLE_VALUE, "open previous settings snapshot");
        auto replacement = s;
        replacement.sendFps = 19;
        bool rejected = false;
        try {
            saveSettings(name, replacement);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected, "a locked settings file reports failed commit");
    }
    check(loadSettings(name, warning).sendFps == 12, "failed commit preserves previous settings");
    check(WritePrivateProfileStringW(L"Encoding", L"Levels", L"3", name.c_str()) != 0,
          "write corrupt settings fixture");
    check(WritePrivateProfileStringW(L"Timing", L"SendFps", L"999", name.c_str()) != 0,
          "write corrupt timing fixture");
    actual = loadSettings(name, warning);
    check(!warning.empty() && actual.encoding == Encoding{} && actual.sendFps == 5 &&
              actual.source == s.source,
          "invalid saved values recover defaults without losing main options");
    std::cout << "Settings: all options, Unicode persistence, atomic replacement/failure and invalid-value "
                 "recovery passed.\n";
}
} // namespace
void fileTransferSelfTest() {
    TestDirectory directory;
    auto run = [&](const char *name, void (*test)(const std::wstring &)) {
        try {
            test(directory.root);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string(name) + ": " + e.what());
        }
    };
    run("boundary files", smallCases);
    run("interrupted resume", resumeCase);
    run("output collision", collisionCase);
    run("identical bytes with different names", sameContentNamesCase);
    run("new destination after window 1", freshDestinationCase);
    run("metadata validation", invalidMetadataCase);
    run("source lock/cancellation", sourceLockAndCancelCase);
    run("changed source", changedSourceCase);
    run("receive cancellation", receiveCancelCase);
    run("final hash rejection", hashMismatchCase);
    run("64-bit offsets", highOffsetCase);
    run("advanced optical profiles", advancedCases);
    run("persistent options", settingsCase);
}
} // namespace cst
