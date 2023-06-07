#ifndef TSCREATE_RGB_TILED_PNG_H
#define TSCREATE_RGB_TILED_PNG_H

#include "tile.h"
#include "rgb_color.h"

#include <vector>
#include <cstddef>

namespace tscreate {
class RgbTiledPng {
    long width;
    long height;
    std::vector<RgbTile> tiles;

public:
    explicit RgbTiledPng(const png::image<png::rgb_pixel>& png);

    [[nodiscard]] size_t size() const { return tiles.size(); }

    [[nodiscard]] long getWidth() const { return width; }

    [[nodiscard]] long getHeight() const { return height; }

    void addTile(const RgbTile& tile);

    [[nodiscard]] const RgbTile& tileAt(long row, long col) const;

    [[nodiscard]] const RgbTile& tileAt(long index) const;
};
} // namespace tscreate

#endif // TSCREATE_RGB_TILED_PNG_H