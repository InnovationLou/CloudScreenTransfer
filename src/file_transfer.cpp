/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "file_transfer.h"
#include "protocol.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <utility>
#include <cstring>
#include <cwctype>

namespace cst {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { close(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void close() noexcept {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        close();
        value = h;
    }
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};
[[noreturn]] void fail(const char *action, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ")");
}
void cancelled(const Cancel &cancel) {
    if (cancel && cancel())
        throw std::runtime_error("Transfer cancelled");
}
std::wstring absolute(const std::wstring &path) {
    if (path.empty())
        throw std::runtime_error("Empty path");
    if (path.compare(0, 4, L"\\\\?\\") == 0)
        return path;
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size)
        fail("GetFullPathName");
    std::vector<wchar_t> buffer(size + 1);
    DWORD n = GetFullPathNameW(path.c_str(), DWORD(buffer.size()), buffer.data(), nullptr);
    if (!n || n >= buffer.size())
        fail("GetFullPathName");
    std::wstring full(buffer.data(), n);
    if (full.compare(0, 2, L"\\\\") == 0)
        return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}
std::wstring displayPath(const std::wstring &path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        return L"\\\\" + path.substr(8);
    return path.compare(0, 4, L"\\\\?\\") == 0 ? path.substr(4) : path;
}
std::wstring join(const std::wstring &dir, const std::wstring &name) {
    return dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
}
std::string utf8(const std::wstring &value) {
    if (value.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0,
                                nullptr, nullptr);
    if (!n)
        fail("Invalid Unicode filename");
    std::string result(size_t(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), &result[0], n,
                             nullptr, nullptr))
        fail("Unicode conversion");
    return result;
}
std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8 filename");
    std::wstring result(size_t(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), &result[0], n))
        fail("UTF-8 conversion");
    return result;
}
std::wstring safeName(const std::string &value) {
    std::wstring name = wide(value);
    // A manifest carries a single basename, never a relative path or device name.
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." || name.back() == L'.' ||
        name.back() == L' ')
        throw std::runtime_error("Unsafe or excessively long output filename");
    for (wchar_t c : name)
        if (c < 32 || c == 127 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos)
            throw std::runtime_error("Manifest filename must be a safe basename");
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ')
        stem.pop_back();
    for (auto &c : stem)
        c = wchar_t(towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
        stem == L"CONOUT$" ||
        (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
         ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' ||
          stem[3] == L'\u00b3')))
        throw std::runtime_error("Reserved Windows device filename");
    if (name.size() >= 5 && name[0] == L'.' && towlower(name[1]) == L'c' && towlower(name[2]) == L's' &&
        towlower(name[3]) == L't' && name[4] == L'-')
        throw std::runtime_error("Filename conflicts with transfer checkpoints");
    return name;
}
uint64_t fileSize(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size))
        fail("GetFileSizeEx");
    if (size.QuadPart < 0)
        throw std::runtime_error("Invalid file size");
    return uint64_t(size.QuadPart);
}
void seek(HANDLE file, uint64_t offset) {
    if (offset > uint64_t(std::numeric_limits<LONGLONG>::max()))
        throw std::runtime_error("File offset exceeds Windows signed 64-bit range");
    LARGE_INTEGER p{};
    p.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(file, p, nullptr, FILE_BEGIN))
        fail("SetFilePointerEx");
}
void readAt(HANDLE file, uint64_t offset, void *buffer, size_t size) {
    seek(file, offset);
    auto *p = static_cast<uint8_t *>(buffer);
    while (size) {
        DWORD n = 0, request = DWORD(std::min<size_t>(size, 1u << 20));
        if (!ReadFile(file, p, request, &n, nullptr))
            fail("ReadFile");
        if (!n)
            throw std::runtime_error("Unexpected end of file");
        p += n;
        size -= n;
    }
}
void writeAt(HANDLE file, uint64_t offset, const void *buffer, size_t size) {
    seek(file, offset);
    const auto *p = static_cast<const uint8_t *>(buffer);
    while (size) {
        DWORD n = 0, request = DWORD(std::min<size_t>(size, 1u << 20));
        if (!WriteFile(file, p, request, &n, nullptr))
            fail("WriteFile");
        if (!n)
            throw std::runtime_error("Zero-length WriteFile");
        p += n;
        size -= n;
    }
}
void flush(HANDLE file) {
    if (!FlushFileBuffers(file))
        fail("FlushFileBuffers");
}
struct Crypto {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    Crypto() {
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            fail("CryptAcquireContext(SHA-256)");
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            DWORD error = GetLastError();
            CryptReleaseContext(provider, 0);
            provider = 0;
            fail("CryptCreateHash(SHA-256)", error);
        }
    }
    ~Crypto() {
        if (hash)
            CryptDestroyHash(hash);
        if (provider)
            CryptReleaseContext(provider, 0);
    }
};
Digest hashFile(HANDLE file, uint64_t size, const Cancel &cancel,
                const std::function<void(uint64_t)> &report) {
    Crypto crypto;
    std::array<uint8_t, 1u << 16> buffer{};
    uint64_t offset = 0;
    if (report)
        report(0);
    while (offset < size) {
        cancelled(cancel);
        DWORD n = DWORD(std::min<uint64_t>(buffer.size(), size - offset));
        readAt(file, offset, buffer.data(), n);
        if (!CryptHashData(crypto.hash, buffer.data(), n, 0))
            fail("CryptHashData");
        offset += n;
        if (report)
            report(offset);
    }
    cancelled(cancel);
    Digest result{};
    DWORD n = DWORD(result.size());
    if (!CryptGetHashParam(crypto.hash, HP_HASHVAL, result.data(), &n, 0) || n != result.size())
        fail("CryptGetHashParam");
    return result;
}
std::wstring hexId(uint64_t id) {
    std::wostringstream s;
    s << std::hex << std::setfill(L'0') << std::setw(16) << id;
    return s.str();
}
std::wstring stateStem(uint64_t id, const std::string &name) {
    // Keep sidecar components short and separate name spellings as well as the
    // protocol's stable file identity; the full name/SHA/size are checked on resume.
    uint32_t nameCrc = crc32(reinterpret_cast<const uint8_t *>(name.data()), name.size());
    std::wostringstream suffix;
    suffix << std::hex << std::setfill(L'0') << std::setw(8) << nameCrc;
    return L".cst-" + hexId(id) + L"-" + suffix.str();
}
bool bit(const std::vector<uint8_t> &bits, unsigned n) {
    return (bits[n / 8] >> (n % 8)) & 1;
}
void setBit(std::vector<uint8_t> &bits, unsigned n) {
    bits[n / 8] |= uint8_t(1u << (n % 8));
}
uint64_t chunks(uint64_t size, const Encoding &e = {}) {
    return size / e.payloadBytes() + (size % e.payloadBytes() != 0);
}
uint64_t windows(uint64_t size, const Encoding &e = {}) {
    uint64_t n = chunks(size, e);
    return n / e.windowChunks() + (n % e.windowChunks() != 0);
}
unsigned inWindow(uint64_t size, uint64_t window, const Encoding &e = {}) {
    uint64_t n = chunks(size, e), first = window * uint64_t(e.windowChunks());
    return first >= n ? 0 : unsigned(std::min<uint64_t>(e.windowChunks(), n - first));
}
unsigned bytesFor(uint64_t size, uint64_t chunk, const Encoding &e = {}) {
    if (chunk >= chunks(size, e))
        return 0;
    return unsigned(std::min<uint64_t>(e.payloadBytes(), size - chunk * uint64_t(e.payloadBytes())));
}
bool frameDone(const std::vector<uint8_t> &bits, unsigned frame, unsigned n, const Encoding &e = {}) {
    unsigned begin = frame * e.slots(), end = std::min(n, begin + e.slots());
    if (begin >= end)
        return false;
    for (unsigned i = begin; i < end; ++i)
        if (!bit(bits, i))
            return false;
    return true;
}
void append32(std::vector<uint8_t> &out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}
void append64(std::vector<uint8_t> &out, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}
uint64_t load64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
uint32_t load32(const uint8_t *p) {
    return uint32_t(load64(std::array<uint8_t, 8>{{p[0], p[1], p[2], p[3], 0, 0, 0, 0}}.data()));
}
bool exists(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return true;
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return false;
    fail("GetFileAttributes", error);
}
void requireRegular(const std::wstring &path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        fail("Inspect transfer file");
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error(
            "Transfer checkpoint/output must be a regular file, not a directory or link");
}
} // namespace

struct Sender::Impl {
    Handle file;
    Manifest meta;
    std::vector<Packet> announcement;
    Progress current;
    ProgressCallback report;
    Cancel cancel;
    uint64_t transfer = 0, window = 0;
    unsigned cursor = 0;
    FrameMask ack{};
    bool announcing = true, finished = false;
    FILETIME originalWrite{};
    explicit Impl(const std::wstring &path, ProgressCallback cb, Cancel stop, Encoding encoding)
        : report(std::move(cb)), cancel(std::move(stop)) {
        encoding.validate();
        meta.encoding = encoding;
        announcement.resize(encoding.manifestSlots());
        std::wstring full = absolute(path);
        file.reset(CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.valid())
            fail("Open source file (must be immutable during transfer)");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info))
            fail("GetFileInformationByHandle");
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            throw std::runtime_error("Source must be a file");
        originalWrite = info.ftLastWriteTime;
        meta.size = fileSize(file.value);
        meta.filename = utf8(full.substr(full.find_last_of(L"\\/") + 1));
        safeName(meta.filename);
        current.total = meta.size;
        current.path = displayPath(full);
        current.hashing = true;
        current.message = L"Calculating source SHA-256";
        meta.sha = hashFile(file.value, meta.size, cancel, [this](uint64_t done) {
            current.done = done;
            notify();
        });
        transfer = transferId(meta);
        ManifestWire wire = encodeManifest(meta);
        for (unsigned i = 0; i < meta.encoding.manifestSlots(); ++i) {
            announcement[i].transfer = transfer;
            announcement[i].index = i;
            announcement[i].data.assign(meta.encoding.payloadBytes(), 0);
            size_t offset = size_t(i) * meta.encoding.payloadBytes();
            std::copy_n(wire.begin() + offset,
                        std::min<size_t>(meta.encoding.payloadBytes(), wire.size() - offset),
                        announcement[i].data.begin());
        }
        current.hashing = false;
        current.done = 0;
        current.message = L"Waiting for receiver metadata acknowledgement";
        notify();
    }
    void notify() {
        if (report)
            report(current);
    }
    void immutable() {
        cancelled(cancel);
        FILETIME last{};
        if (!GetFileTime(file.value, nullptr, nullptr, &last))
            fail("GetFileTime");
        if (fileSize(file.value) != meta.size || CompareFileTime(&last, &originalWrite) != 0)
            throw std::runtime_error("Source file changed during transfer");
    }
    uint64_t acknowledged() const {
        uint64_t result = std::min(meta.size, window * meta.encoding.windowBytes());
        unsigned n = inWindow(meta.size, window, meta.encoding);
        for (unsigned f = 0; f < meta.encoding.windowFrames; ++f)
            if ((ack[f / 8] >> (f % 8)) & 1) {
                unsigned begin = f * meta.encoding.slots(), end = std::min(n, begin + meta.encoding.slots());
                for (unsigned i = begin; i < end; ++i)
                    result += bytesFor(meta.size, window * uint64_t(meta.encoding.windowChunks()) + i,
                                       meta.encoding);
            }
        return result;
    }
    WireFrame next(uint32_t sequence) {
        immutable();
        WireFrame frame;
        frame.info = {transfer, window, sequence, finished, meta.encoding};
        frame.packets.resize(meta.encoding.slots());
        for (auto &packet : frame.packets)
            packet.data.assign(meta.encoding.payloadBytes(), 0);
        if (announcing || finished || !meta.size) {
            for (unsigned i = 0; i < meta.encoding.slots(); ++i)
                frame.packets[i] = announcement[i % meta.encoding.manifestSlots()];
            return frame;
        }
        unsigned n = inWindow(meta.size, window, meta.encoding),
                 count = n / meta.encoding.slots() + (n % meta.encoding.slots() != 0);
        unsigned chosen = count;
        for (unsigned attempt = 0; attempt < count; ++attempt) {
            unsigned f = cursor++ % count;
            if (!((ack[f / 8] >> (f % 8)) & 1)) {
                chosen = f;
                break;
            }
        }
        // Once all frame bits are acknowledged, keep announcing while waiting for the
        // receiver's next-window feedback or final verified D command.
        if (chosen == count) {
            for (unsigned i = 0; i < meta.encoding.slots(); ++i)
                frame.packets[i] = announcement[i % meta.encoding.manifestSlots()];
            return frame;
        }
        uint64_t first =
            window * uint64_t(meta.encoding.windowChunks()) + chosen * uint64_t(meta.encoding.slots());
        size_t amount =
            size_t(std::min<uint64_t>(meta.encoding.slots() * uint64_t(meta.encoding.payloadBytes()),
                                      meta.size - first * meta.encoding.payloadBytes()));
        std::vector<uint8_t> bytes(size_t(meta.encoding.slots()) * meta.encoding.payloadBytes(), 0);
        readAt(file.value, first * meta.encoding.payloadBytes(), bytes.data(), amount);
        for (unsigned i = 0; i < meta.encoding.slots(); ++i) {
            uint64_t chunk = first + i;
            if (chunk >= chunks(meta.size, meta.encoding)) {
                frame.packets[i] = announcement[i % meta.encoding.manifestSlots()];
                continue;
            }
            Packet &p = frame.packets[i];
            p.transfer = transfer;
            p.index = meta.encoding.manifestSlots() + chunk;
            unsigned len = bytesFor(meta.size, chunk, meta.encoding);
            std::copy_n(bytes.data() + i * meta.encoding.payloadBytes(), len, p.data.begin());
        }
        return frame;
    }
    void feedback(const Feedback &command) {
        cancelled(cancel);
        if (command.operation == 'M' && (command.transfer == 0 || command.transfer == transfer)) {
            announcing = true;
            ack.fill(0);
            cursor = 0;
            current.message = L"Resending metadata";
            notify();
            return;
        }
        if (command.transfer != transfer || finished)
            return;
        if (command.operation == 'D') {
            immutable();
            finished = true;
            announcing = false;
            current.newBytes = meta.size - current.done;
            current.done = meta.size;
            current.complete = true;
            current.message = L"Receiver verified SHA-256; transfer complete";
            notify();
            current.newBytes = 0;
            return;
        }
        // An explicit metadata resynchronization can mean a different destination
        // directory with no checkpoint. Its first P is authoritative even when it
        // asks for an earlier window. Outside announce, ignore stale older P lines.
        if (command.operation != 'P' || command.window >= windows(meta.size, meta.encoding) ||
            (command.window < window && !announcing))
            return;
        if (command.window != window) {
            window = command.window;
            ack.fill(0);
            cursor = 0;
        }
        // The receiver's bitmap is authoritative, including after a partial-window
        // restart. Replacing it lets a rolled-back checkpoint request lost chunks.
        ack = command.mask;
        announcing = false;
        uint64_t done = acknowledged();
        current.newBytes = done > current.done ? done - current.done : 0;
        current.done = done;
        current.message = L"Sending requested frames";
        notify();
        current.newBytes = 0;
    }
};
Sender::Sender(const std::wstring &p, ProgressCallback cb, Cancel cancel, Encoding encoding)
    : impl(new Impl(p, std::move(cb), std::move(cancel), encoding)) {}
Sender::~Sender() = default;
uint64_t Sender::id() const {
    return impl->transfer;
}
const Manifest &Sender::manifest() const {
    return impl->meta;
}
WireFrame Sender::next(uint32_t sequence) {
    return impl->next(sequence);
}
void Sender::feedback(const Feedback &f) {
    impl->feedback(f);
}
Progress Sender::progress() const {
    return impl->current;
}
bool Sender::complete() const {
    return impl->finished;
}

struct Receiver::Impl {
    std::wstring directory, partPath, statePath, finalName;
    Handle part;
    Manifest meta;
    std::vector<Packet> announcement;
    unsigned announcementMask = 0;
    std::vector<uint8_t> received = std::vector<uint8_t>(WindowChunks / 8, 0);
    Progress current;
    ProgressCallback report;
    Cancel cancel;
    uint64_t transfer = 0, candidate = 0, window = 0, lastCheckpoint = 0;
    bool candidateKnown = false, known = false, finished = false, ownsState = false, dirty = false;
    uint32_t stateStatus = 0; // 0 receiving, 1 complete, 2 verified part ready to rename
    explicit Impl(const std::wstring &output, ProgressCallback cb, Cancel stop)
        : directory(absolute(output)), report(std::move(cb)), cancel(std::move(stop)) {
        DWORD attrs = GetFileAttributesW(directory.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                fail("Output directory", error);
            if (!CreateDirectoryW(directory.c_str(), nullptr))
                fail("Create output directory");
            attrs = GetFileAttributesW(directory.c_str());
        }
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Output path must be a directory");
        current.message = L"Waiting for file metadata";
        current.path = displayPath(directory);
        notify();
    }
    ~Impl() {
        try {
            if (known && part.valid() && dirty)
                checkpoint();
        } catch (...) {
        }
    }
    void notify() {
        if (report)
            report(current);
    }
    uint64_t receivedBytes() const {
        uint64_t result = std::min(meta.size, window * meta.encoding.windowBytes());
        unsigned n = inWindow(meta.size, window, meta.encoding);
        for (unsigned i = 0; i < n; ++i)
            if (bit(received, i))
                result +=
                    bytesFor(meta.size, window * uint64_t(meta.encoding.windowChunks()) + i, meta.encoding);
        return result;
    }
    std::vector<uint8_t> stateBytes() const {
        std::vector<uint8_t> out{'C', 'S', 'T', 'S', 'T', 'A', 'T', 'E'};
        append32(out, 1);
        append32(out, stateStatus);
        append64(out, transfer);
        append64(out, meta.size);
        out.insert(out.end(), meta.sha.begin(), meta.sha.end());
        append64(out, window);
        out.insert(out.end(), received.begin(), received.end());
        std::string final = utf8(finalName);
        append32(out, uint32_t(meta.filename.size()));
        append32(out, uint32_t(final.size()));
        out.insert(out.end(), meta.filename.begin(), meta.filename.end());
        out.insert(out.end(), final.begin(), final.end());
        append32(out, crc32(out.data(), out.size()));
        return out;
    }
    void checkpoint() {
        if (!known)
            return;
        // Data reaches stable storage before its bitmap can be acknowledged as durable.
        if (part.valid())
            flush(part.value);
        std::vector<uint8_t> bytes = stateBytes();
        std::wstring temp;
        Handle file;
        static std::atomic<unsigned long> counter{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            temp = statePath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                   std::to_wstring(++counter) + L".new";
            file.reset(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
            if (file.valid())
                break;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                fail("Create checkpoint temporary file");
        }
        if (!file.valid())
            throw std::runtime_error("Cannot create checkpoint temporary file");
        try {
            writeAt(file.value, 0, bytes.data(), bytes.size());
            flush(file.value);
            file.close();
            DWORD flags = MOVEFILE_WRITE_THROUGH | (ownsState ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileExW(temp.c_str(), statePath.c_str(), flags))
                fail("Commit checkpoint");
            ownsState = true;
            dirty = false;
            lastCheckpoint = GetTickCount64();
        } catch (...) {
            file.close();
            DeleteFileW(temp.c_str());
            throw;
        }
    }
    bool restore() {
        if (!exists(statePath))
            return false;
        requireRegular(statePath);
        Handle file(CreateFileW(statePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid())
            fail("Open transfer checkpoint");
        uint64_t n = fileSize(file.value);
        const size_t fixed = 8 + 4 + 4 + 8 + 8 + 32 + 8 + received.size() + 4 + 4 + 4;
        if (n < fixed || n > fixed + 4096)
            throw std::runtime_error("Invalid existing transfer checkpoint; refusing to overwrite it");
        std::vector<uint8_t> bytes(size_t(n), 0);
        readAt(file.value, 0, bytes.data(), bytes.size());
        if (std::memcmp(bytes.data(), "CSTSTATE", 8) != 0 || load32(bytes.data() + 8) != 1 ||
            load32(bytes.data() + bytes.size() - 4) != crc32(bytes.data(), bytes.size() - 4))
            throw std::runtime_error("Corrupt existing transfer checkpoint; refusing to overwrite it");
        uint32_t status = load32(bytes.data() + 12);
        uint64_t id = load64(bytes.data() + 16), size = load64(bytes.data() + 24),
                 w = load64(bytes.data() + 64);
        if (status > 2 || id != transfer || size != meta.size ||
            std::memcmp(bytes.data() + 32, meta.sha.data(), 32) != 0 || w > windows(meta.size, meta.encoding))
            throw std::runtime_error("Checkpoint does not match incoming file");
        size_t namesAt = 72 + received.size();
        uint32_t nameLen = load32(bytes.data() + namesAt), finalLen = load32(bytes.data() + namesAt + 4);
        if (uint64_t(fixed) + nameLen + finalLen != bytes.size())
            throw std::runtime_error("Invalid checkpoint name lengths");
        std::string name(reinterpret_cast<const char *>(bytes.data() + namesAt + 8), nameLen);
        if (name != meta.filename)
            throw std::runtime_error("Checkpoint filename differs from incoming manifest");
        std::string final(reinterpret_cast<const char *>(bytes.data() + namesAt + 8 + nameLen), finalLen);
        finalName = final.empty() ? std::wstring() : safeName(final);
        if (status && finalName.empty())
            throw std::runtime_error("Completed checkpoint lacks output filename");
        window = w;
        stateStatus = status;
        std::copy_n(bytes.data() + 72, received.size(), received.begin());
        unsigned valid = inWindow(meta.size, window, meta.encoding);
        for (unsigned i = valid; i < meta.encoding.windowChunks(); ++i)
            if (bit(received, i))
                throw std::runtime_error("Checkpoint bitmap exceeds file size");
        if (status && window != windows(meta.size, meta.encoding))
            throw std::runtime_error("Completed checkpoint has incomplete windows");
        ownsState = true;
        current.resumed = true;
        current.message = L"Resuming validated checkpoint";
        return true;
    }
    void hashProgress(uint64_t done) {
        current.hashing = true;
        current.done = done;
        current.newBytes = 0;
        current.message = L"Verifying received file SHA-256";
        notify();
    }
    void verifyCompleted() {
        std::wstring path = join(directory, finalName);
        requireRegular(path);
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.valid())
            fail("Open completed file for resume verification");
        if (fileSize(file.value) != meta.size ||
            hashFile(file.value, meta.size, cancel, [this](uint64_t done) { hashProgress(done); }) !=
                meta.sha)
            throw std::runtime_error("Previously completed output failed SHA-256 verification");
        stateStatus = 1;
        checkpoint();
        finished = true;
        current.hashing = false;
        current.done = meta.size;
        current.complete = true;
        current.path = displayPath(path);
        current.message = L"Existing completed file verified; transfer complete";
        notify();
    }
    void finish() {
        checkpoint();
        if (fileSize(part.value) != meta.size)
            throw std::runtime_error("Received temporary file has incorrect size");
        Digest actual =
            hashFile(part.value, meta.size, cancel, [this](uint64_t done) { hashProgress(done); });
        current.hashing = false;
        current.done = receivedBytes();
        if (actual != meta.sha) {
            current.message = L"SHA-256 mismatch; transfer has not completed";
            notify();
            throw std::runtime_error("Received file SHA-256 mismatch");
        }
        // Persist the proposed final name before rename. If power fails after rename,
        // resume verifies that named output instead of treating its absence as success.
        std::wstring base = safeName(meta.filename);
        for (unsigned number = 0; number < 10000; ++number) {
            std::wstring suffix = number ? L" (" + std::to_wstring(number) + L")" : L"";
            size_t dot = base.find_last_of(L'.');
            if (dot == 0 || dot == std::wstring::npos)
                dot = base.size();
            std::wstring extension = base.substr(dot), stem = base.substr(0, dot);
            if (extension.size() + suffix.size() >= 255) {
                stem = base;
                extension.clear();
            }
            if (stem.size() + suffix.size() + extension.size() > 255) {
                size_t maxStem = 255 - suffix.size() - extension.size();
                if (maxStem && stem[maxStem - 1] >= 0xd800 && stem[maxStem - 1] <= 0xdbff)
                    --maxStem;
                stem.resize(maxStem);
            }
            finalName = stem + suffix + extension;
            std::wstring target = join(directory, finalName);
            if (exists(target))
                continue;
            stateStatus = 2;
            dirty = true;
            checkpoint();
            part.close();
            if (MoveFileExW(partPath.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                stateStatus = 1;
                dirty = true;
                checkpoint();
                finished = true;
                current.hashing = false;
                current.done = meta.size;
                current.newBytes = 0;
                current.complete = true;
                current.path = displayPath(target);
                current.message = L"SHA-256 verified; file saved";
                notify();
                return;
            }
            DWORD error = GetLastError();
            part.reset(CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!part.valid())
                fail("Reopen received file after rename failure");
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                fail("Rename received file without overwrite", error);
        }
        throw std::runtime_error("Cannot select an unused output filename");
    }
    void start(const Manifest &incoming, uint64_t id) {
        safeName(incoming.filename);
        if (incoming.size > uint64_t(std::numeric_limits<LONGLONG>::max()) || transferId(incoming) != id)
            throw std::runtime_error("Invalid file size or transfer identity");
        meta = incoming;
        received.assign(meta.encoding.windowChunks() / 8, 0);
        transfer = id;
        current.total = meta.size;
        std::wstring stem = stateStem(transfer, meta.filename);
        partPath = join(directory, stem + L".part");
        statePath = join(directory, stem + L".state");
        bool resumed = restore();
        known = true;
        if (resumed && (stateStatus == 1 || (stateStatus == 2 && !exists(partPath)))) {
            verifyCompleted();
            return;
        }
        if (resumed)
            requireRegular(partPath);
        part.reset(CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               resumed ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!part.valid())
            fail(resumed ? "Open received temporary file"
                         : "Create received temporary file (existing files are preserved)");
        if (resumed) {
            uint64_t required = std::min(meta.size, window * meta.encoding.windowBytes());
            unsigned n = inWindow(meta.size, window, meta.encoding);
            for (unsigned i = 0; i < n; ++i)
                if (bit(received, i))
                    required = std::max(
                        required,
                        (window * uint64_t(meta.encoding.windowChunks()) + i) * meta.encoding.payloadBytes() +
                            bytesFor(meta.size, window * uint64_t(meta.encoding.windowChunks()) + i,
                                     meta.encoding));
            if (fileSize(part.value) < required || fileSize(part.value) > meta.size)
                throw std::runtime_error("Temporary file size does not match its checkpoint");
        }
        current.done = receivedBytes();
        current.path = displayPath(partPath);
        current.message = resumed ? L"Resuming received file" : L"Receiving file";
        dirty = true;
        checkpoint();
        notify();
        if (window == windows(meta.size, meta.encoding))
            finish();
    }
    void ingest(const DecodedFrame &frame) {
        try {
            cancelled(cancel);
            if (finished)
                return;
            frame.info.encoding.validate();
            if (known && frame.info.encoding != meta.encoding)
                return;
            uint64_t newBytes = 0;
            for (const Packet &p : frame.packets) {
                cancelled(cancel);
                if (p.transfer != frame.info.transfer || (known && p.transfer != transfer) ||
                    p.data.size() != frame.info.encoding.payloadBytes())
                    continue;
                if (p.index < frame.info.encoding.manifestSlots()) {
                    if (known)
                        continue;
                    if (!candidateKnown || candidate != p.transfer || meta.encoding != frame.info.encoding) {
                        candidate = p.transfer;
                        meta.encoding = frame.info.encoding;
                        announcement.assign(meta.encoding.manifestSlots(), Packet{});
                        candidateKnown = true;
                        announcementMask = 0;
                    }
                    announcement[size_t(p.index)] = p;
                    announcementMask |= 1u << unsigned(p.index);
                    if (announcementMask == (1u << meta.encoding.manifestSlots()) - 1) {
                        Manifest decoded;
                        try {
                            ManifestWire wire{};
                            for (unsigned i = 0; i < meta.encoding.manifestSlots(); ++i)
                                std::copy_n(
                                    announcement[i].data.begin(),
                                    std::min<size_t>(meta.encoding.payloadBytes(),
                                                     wire.size() - size_t(i) * meta.encoding.payloadBytes()),
                                    wire.begin() + size_t(i) * meta.encoding.payloadBytes());
                            const auto &last = announcement.back().data;
                            size_t used =
                                wire.size() - (announcement.size() - 1) * meta.encoding.payloadBytes();
                            if (!std::all_of(last.begin() + used, last.end(),
                                             [](uint8_t n) { return n == 0; }))
                                throw std::runtime_error("Nonzero metadata packet padding");
                            decoded = decodeManifest(wire);
                            safeName(decoded.filename);
                            if (decoded.encoding != frame.info.encoding || transferId(decoded) != candidate ||
                                decoded.size > uint64_t(std::numeric_limits<LONGLONG>::max()))
                                throw std::runtime_error("Invalid manifest identity or size");
                        } catch (const std::exception &) {
                            announcementMask = 0;
                            current.message = L"Invalid metadata; requesting another announcement";
                            notify();
                            continue;
                        }
                        start(decoded, candidate);
                        if (finished)
                            return;
                    }
                    continue;
                }
                if (!known || frame.info.window != window)
                    continue;
                uint64_t chunk = p.index - meta.encoding.manifestSlots();
                if (chunk >= chunks(meta.size, meta.encoding) ||
                    chunk / meta.encoding.windowChunks() != window)
                    continue;
                unsigned local = unsigned(chunk % meta.encoding.windowChunks());
                if (bit(received, local))
                    continue;
                unsigned len = bytesFor(meta.size, chunk, meta.encoding);
                writeAt(part.value, chunk * meta.encoding.payloadBytes(), p.data.data(), len);
                setBit(received, local);
                dirty = true;
                newBytes += len;
            }
            if (!known || finished)
                return;
            current.newBytes = newBytes;
            current.done = receivedBytes();
            current.message = L"Receiving file";
            notify();
            current.newBytes = 0;
            unsigned n = inWindow(meta.size, window, meta.encoding);
            bool all = true;
            for (unsigned i = 0; i < n; ++i)
                if (!bit(received, i)) {
                    all = false;
                    break;
                }
            if (all) {
                // Commit bytes and completed prefix as one durable checkpoint.
                ++window;
                std::fill(received.begin(), received.end(), 0);
                dirty = true;
                current.done = receivedBytes();
                checkpoint();
                if (window == windows(meta.size, meta.encoding))
                    finish();
                else {
                    current.message = L"Window saved; requesting next window";
                    notify();
                }
            } else if (dirty && GetTickCount64() - lastCheckpoint >= 2000)
                checkpoint();
        } catch (...) {
            try {
                if (known && part.valid() && dirty)
                    checkpoint();
            } catch (...) {
            }
            throw;
        }
    }
    Feedback feedback() const {
        Feedback out;
        if (!known)
            return out;
        out.transfer = transfer;
        out.window = window;
        if (finished) {
            out.operation = 'D';
            out.window = 0;
            return out;
        }
        out.operation = 'P';
        unsigned n = inWindow(meta.size, window, meta.encoding);
        for (unsigned f = 0; f < meta.encoding.windowFrames; ++f)
            if (frameDone(received, f, n, meta.encoding))
                out.mask[f / 8] |= uint8_t(1u << (f % 8));
        return out;
    }
};
Receiver::Receiver(const std::wstring &output, ProgressCallback cb, Cancel cancel)
    : impl(new Impl(output, std::move(cb), std::move(cancel))) {}
Receiver::~Receiver() = default;
void Receiver::ingest(const DecodedFrame &frame) {
    impl->ingest(frame);
}
void Receiver::save() {
    // Explicit stop must surface disk/flush/checkpoint failures. Saving bytes
    // already accepted is allowed even when the session's Cancel flag is set.
    if (impl->known && impl->part.valid() && impl->dirty)
        impl->checkpoint();
}
Feedback Receiver::feedback() const {
    return impl->feedback();
}
Progress Receiver::progress() const {
    return impl->current;
}
bool Receiver::complete() const {
    return impl->finished;
}
} // namespace cst
