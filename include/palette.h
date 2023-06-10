#ifndef TSCREATE_PALETTE_H
#define TSCREATE_PALETTE_H

#include "rgb_color.h"

#include <png.hpp>
#include <unordered_set>
#include <deque>

namespace tscreate {
constexpr int PAL_SIZE_4BPP = 16;

class Palette {
    std::unordered_set<RgbColor> index;
    std::deque<RgbColor> colors;

public:
    explicit Palette(const RgbColor& transparencyColor) {
        // TODO : we should not add this until the very end, will require code change in many places
        index.insert(transparencyColor);
        colors.push_back(transparencyColor);
    }

    bool addColorAtStart(const RgbColor& color);

    bool addColorAtEnd(const RgbColor& color);

    RgbColor colorAt(int i);

    [[nodiscard]] size_t remainingColors() const;

    [[nodiscard]] const std::unordered_set<RgbColor>& getIndex() const { return index; }
};
} // namespace tscreate

#endif // TSCREATE_PALLETE_H