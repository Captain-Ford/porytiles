#ifndef TSCREATE_TILE_H
#define TSCREATE_TILE_H

#include <png.hpp>

namespace tscreate {
extern const png::uint_32 TILE_DIMENSION;

template<typename T>
class Tile {
    T pixels[8][8];

public:
    Tile(T defaultValue) {
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                this->pixels[i][j] = defaultValue;
            }
        }
    }

    Tile(const Tile& other) {
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                this->pixels[i][j] = other.pixels[i][j];
            }
        }
    }
};

typedef Tile<png::rgb_pixel> RgbTile;
typedef Tile<png::uint_32> IndexedTile;
}

#endif // TSCREATE_TILE_H