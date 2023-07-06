#include "png_frontend.h"

#include <png.hpp>

#include "types.h"

namespace porytiles {

DecompiledTileset importTilesFrom(const png::image<png::rgba_pixel>& png) {
    DecompiledTileset decompiledTiles;

    std::size_t pngWidthInTiles = png.get_width() / TILE_SIDE_LENGTH;
    std::size_t pngHeightInTiles = png.get_height() / TILE_SIDE_LENGTH;

    for (size_t tileIndex = 0; tileIndex < pngWidthInTiles * pngHeightInTiles; tileIndex++) {
        size_t tileRow = tileIndex / pngWidthInTiles;
        size_t tileCol = tileIndex % pngWidthInTiles;
        RGBATile tile{};
        for (size_t pixelIndex = 0; pixelIndex < TILE_NUM_PIX; pixelIndex++) {
            size_t pixelRow = (tileRow * TILE_SIDE_LENGTH) + (pixelIndex / TILE_SIDE_LENGTH);
            size_t pixelCol = (tileCol * TILE_SIDE_LENGTH) + (pixelIndex % TILE_SIDE_LENGTH);
            tile.pixels[pixelIndex].red = png[pixelRow][pixelCol].red;
            tile.pixels[pixelIndex].green = png[pixelRow][pixelCol].green;
            tile.pixels[pixelIndex].blue = png[pixelRow][pixelCol].blue;
            tile.pixels[pixelIndex].alpha = png[pixelRow][pixelCol].alpha;
        }
        decompiledTiles.tiles.push_back(tile);
    }
    return decompiledTiles;
}
TEST_CASE("png_frontend::decompiledTilesFrom should read an RGBA PNG into a DecompiledTileset in tile-wise left-to-right, top-to-bottom order") {
    REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_1.png"));
    png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_1.png"};

    DecompiledTileset tiles = importTilesFrom(png1);

    // Tile 0 should have blue stripe from top left to bottom right
    CHECK(tiles.tiles[0].pixels[0] == RGBA_BLUE);
    CHECK(tiles.tiles[0].pixels[9] == RGBA_BLUE);
    CHECK(tiles.tiles[0].pixels[54] == RGBA_BLUE);
    CHECK(tiles.tiles[0].pixels[63] == RGBA_BLUE);
    CHECK(tiles.tiles[0].pixels[1] == RGBA_MAGENTA);

    // Tile 1 should have red stripe from top right to bottom left
    CHECK(tiles.tiles[1].pixels[7] == RGBA_RED);
    CHECK(tiles.tiles[1].pixels[14] == RGBA_RED);
    CHECK(tiles.tiles[1].pixels[49] == RGBA_RED);
    CHECK(tiles.tiles[1].pixels[56] == RGBA_RED);
    CHECK(tiles.tiles[1].pixels[0] == RGBA_MAGENTA);

    // Tile 2 should have green stripe from top right to bottom left
    CHECK(tiles.tiles[2].pixels[7] == RGBA_GREEN);
    CHECK(tiles.tiles[2].pixels[14] == RGBA_GREEN);
    CHECK(tiles.tiles[2].pixels[49] == RGBA_GREEN);
    CHECK(tiles.tiles[2].pixels[56] == RGBA_GREEN);
    CHECK(tiles.tiles[2].pixels[0] == RGBA_MAGENTA);

    // Tile 3 should have yellow stripe from top left to bottom right
    CHECK(tiles.tiles[3].pixels[0] == RGBA_YELLOW);
    CHECK(tiles.tiles[3].pixels[9] == RGBA_YELLOW);
    CHECK(tiles.tiles[3].pixels[54] == RGBA_YELLOW);
    CHECK(tiles.tiles[3].pixels[63] == RGBA_YELLOW);
    CHECK(tiles.tiles[3].pixels[1] == RGBA_MAGENTA);
}

}
