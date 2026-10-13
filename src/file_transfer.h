/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once
#include "common.h"
#include <memory>
namespace cst {
class Sender {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Sender(const std::wstring &source, ProgressCallback progress = {}, Cancel cancel = {});
    ~Sender();
    Sender(const Sender &) = delete;
    uint64_t id() const;
    const Manifest &manifest() const;
    WireFrame next(uint32_t sequence);
    void feedback(const Feedback &command);
    Progress progress() const;
    bool complete() const;
};
class Receiver {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Receiver(const std::wstring &outputDirectory, ProgressCallback progress = {}, Cancel cancel = {});
    ~Receiver();
    Receiver(const Receiver &) = delete;
    void ingest(const DecodedFrame &frame);
    void save(); // Flush received bytes and persist the resumable checkpoint; may throw.
    Feedback feedback() const;
    Progress progress() const;
    bool complete() const;
};
} // namespace cst
