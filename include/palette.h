#ifndef TSCREATE_PALETTE_H
#define TSCREATE_PALETTE_H

#include "pixel_comparators.h"
#include "rgb_color.h"

#include <png.hpp>
#include <unordered_set>
#include <vector>

namespace tscreate {
extern const int PAL_SIZE_4BPP;

class Palette {
    std::unordered_set<tscreate::RgbColor> colors;
    std::vector<tscreate::RgbColor> index;

public:
    explicit Palette(const tscreate::RgbColor& transparencyColor) {
        colors.insert(transparencyColor);
        index.push_back(transparencyColor);
    }

    bool addColor(const tscreate::RgbColor& color);

    [[nodiscard]] auto remainingColors() const;
};
} // namespace tscreate

#endif // TSCREATE_PALLETE_H