/* This code is subject to the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "encoding.h"
#include <stdexcept>

namespace cst {
unsigned Encoding::bits() const {
    unsigned result = 0;
    for (unsigned value = levels; value > 1; value >>= 1)
        ++result;
    return result;
}
unsigned Encoding::slots() const {
    return ((width - 48) / cell) * ((height - 108) / cell) * bits() / (8 * 255);
}
bool Encoding::operator==(const Encoding &o) const {
    return width == o.width && height == o.height && cell == o.cell && levels == o.levels &&
           parity == o.parity && windowFrames == o.windowFrames;
}
void Encoding::validate() const {
    if (width < MinCanvasWidth || width > 1920 || height < MinCanvasHeight || height > 1080 || cell < 2 ||
        cell > 8 || (levels != 2 && levels != 4 && levels != 8 && levels != 16) ||
        (parity != 16 && parity != 24 && parity != 32 && parity != 48 && parity != 64) || windowFrames < 16 ||
        windowFrames > 256 || windowFrames % 16)
        throw std::invalid_argument("Encoding values are outside the supported ranges");
    if (slots() < manifestSlots() || slots() > 1024)
        throw std::invalid_argument("This canvas/cell/gray combination cannot hold the file information");
}
} // namespace cst
