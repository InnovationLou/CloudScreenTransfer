/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#pragma once

namespace cst::diagnostics {
void install() noexcept;
void stage(const char *name) noexcept;
const char *stage() noexcept;
void append(const char *message) noexcept;
void exception(const char *type, const char *message) noexcept;
} // namespace cst::diagnostics
