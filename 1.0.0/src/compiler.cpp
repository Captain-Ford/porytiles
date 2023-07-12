#include "compiler.h"

#include <png.hpp>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <tuple>

#include "doctest.h"
#include "config.h"
#include "ptexception.h"
#include "constants.h"
#include "types.h"
#include "png_frontend.h"

/*
 * Some of the types we need are extremely verbose and confusing, so here let's define some better names to make the
 * code a bit more readable.
 */
using ColorSet = std::bitset<240>;
using DecompiledIndex = std::size_t;
using IndexedNormTile = std::pair<DecompiledIndex, porytiles::NormalizedTile>;
using IndexedNormTileWithColorSet = std::tuple<DecompiledIndex, porytiles::NormalizedTile, ColorSet>;

namespace porytiles {

// TODO : change this to receive CompilerContext once I have made that type available
static size_t insertRGBA(const Config& config, NormalizedPalette& palette, RGBA32 rgba) {
    /*
     * Insert an rgba32 color into a normalized palette. The color will be converted to bgr15 format in the process,
     * and possibly deduped (depending on user settings). Transparent alpha pixels will be treated as transparent, as
     * will pixels that are of transparent color (again, set by the user but default to magenta). Fails if a tile
     * contains too many unique colors or if an invalid alpha value is detected.
     */
    if (rgba.alpha == ALPHA_TRANSPARENT || rgba == config.transparencyColor) {
        return 0;
    }
    else if (rgba.alpha == ALPHA_OPAQUE) {
        /*
         * TODO : we lose color precision here, it would be nice to warn the user if two distinct RGBA colors they used
         * in the master sheet are going to collapse to one BGR color on the GBA. This should default fail the build,
         * but a compiler flag '--ignore-color-precision-loss' would disable this warning
         */
        auto bgr = rgbaToBgr(rgba);
        auto itrAtBgr = std::find(std::begin(palette.colors) + 1, std::begin(palette.colors) + palette.size, bgr);
        auto bgrPosInPalette = itrAtBgr - std::begin(palette.colors);
        if (bgrPosInPalette == palette.size) {
            // palette size will grow as we add to it
            if (palette.size == PAL_SIZE) {
                // TODO : better error context
                throw PtException{"too many unique colors in tile"};
            }
            palette.colors[palette.size++] = bgr;
        }
        return bgrPosInPalette;
    }
    // TODO : better error context
    throw PtException{"invalid alpha value: " + std::to_string(rgba.alpha)};
}

static NormalizedTile candidate(const Config& config, const RGBATile& rgba, bool hFlip, bool vFlip) {
    /*
     * NOTE: This only produces a _candidate_ normalized tile (a different choice of hFlip/vFlip might be the normal
     * form). We'll use this to generate candidates to find the true normal form.
     */
    NormalizedTile candidateTile{};
    // Size is 1 to account for eventual transparent color in first palette slot
    candidateTile.palette.size = 1;
    // TODO : same color precision note as above in insertRGBA
    candidateTile.palette.colors[0] = rgbaToBgr(config.transparencyColor);
    candidateTile.hFlip = hFlip;
    candidateTile.vFlip = vFlip;

    for (std::size_t row = 0; row < TILE_SIDE_LENGTH; row++) {
        for (std::size_t col = 0; col < TILE_SIDE_LENGTH; col++) {
            std::size_t rowWithFlip = vFlip ? TILE_SIDE_LENGTH - 1 - row : row;
            std::size_t colWithFlip = hFlip ? TILE_SIDE_LENGTH - 1 - col : col;
            candidateTile.setPixel(row, col,
                                   insertRGBA(config, candidateTile.palette, rgba.getPixel(rowWithFlip, colWithFlip)));
        }
    }

    return candidateTile;
}

static NormalizedTile normalize(const Config& config, const RGBATile& rgba) {
    /*
     * Normalize the given tile by checking each of the 4 possible flip states, and choosing the one that comes first in
     * "lexicographic" order, where this order is determined by the std::array spaceship operator.
     */
    auto noFlipsTile = candidate(config, rgba, false, false);

    // Short-circuit because transparent tiles are common in metatiles and trivially in normal form.
    if (noFlipsTile.transparent()) {
        return noFlipsTile;
    }

    auto hFlipTile = candidate(config, rgba, true, false);
    auto vFlipTile = candidate(config, rgba, false, true);
    auto bothFlipsTile = candidate(config, rgba, true, true);

    std::array<const NormalizedTile*, 4> candidates = {&noFlipsTile, &hFlipTile, &vFlipTile, &bothFlipsTile};
    auto normalizedTile = std::min_element(std::begin(candidates), std::end(candidates),
                                           [](auto tile1, auto tile2) { return tile1->pixels < tile2->pixels; });
    return **normalizedTile;
}

static std::vector<IndexedNormTile>
normalizeDecompTiles(const Config& config, const DecompiledTileset& decompiledTileset) {
    /*
     * For each tile in the decomp tileset, normalize it and tag it with its index in the decomp tileset.
     */
    std::vector<IndexedNormTile> normalizedTiles;
    DecompiledIndex decompiledIndex = 0;
    for (auto const& tile: decompiledTileset.tiles) {
        auto normalizedTile = normalize(config, tile);
        normalizedTiles.emplace_back(decompiledIndex++, normalizedTile);
    }
    return normalizedTiles;
}

static std::unordered_map<BGR15, std::size_t>
buildColorIndexMap(const Config& config, const std::vector<IndexedNormTile>& normalizedTiles) {
    /*
     * Iterate over every color in each tile's NormalizedPalette, adding it to the map if not already present. We end up
     * with a map of colors to unique indexes.
     */
    std::unordered_map<BGR15, std::size_t> colorIndexes;
    std::size_t colorIndex = 0;
    for (const auto& [_, normalizedTile]: normalizedTiles) {
        // i starts at 1, since first color in each palette is the transparency color
        for (int i = 1; i < normalizedTile.palette.size; i++) {
            bool inserted = colorIndexes.insert(std::pair{normalizedTile.palette.colors[i], colorIndex}).second;
            if (inserted) {
                colorIndex++;
            }
        }
    }
    // TODO : this needs to take into account secondary tilesets, so `numPalettesTotal - numPalettesInPrimary'
    if (colorIndex > (PAL_SIZE - 1) * config.numPalettesInPrimary) {
        // TODO : better error context
        throw PtException{"too many unique colors"};
    }

    return colorIndexes;
}

static ColorSet
toColorSet(const std::unordered_map<BGR15, std::size_t>& colorIndexMap, const NormalizedPalette& palette) {
    /*
     * Set a color set based on a given palette. Each bit in the ColorSet represents if the color at the given index in
     * the supplied color map was present in the palette. E.g. suppose the color map has 12 unique colors. The supplied
     * palette has two colors in it, which correspond to index 2 and index 11. The ColorSet bitset would be:
     * 0010 0000 0001
     */
    ColorSet colorSet;
    // starts at 1, skip the transparent color at slot 0 in the normalized palette
    for (int i = 1; i < palette.size; i++) {
        colorSet.set(colorIndexMap.at(palette.colors[i]));
    }
    return colorSet;
}

static std::pair<std::vector<IndexedNormTileWithColorSet>, std::unordered_set<ColorSet>>
matchNormalizedWithColorSets(const std::unordered_map<BGR15, std::size_t>& colorIndexMap, const std::vector<IndexedNormTile>& indexedNormalizedTiles) {
    std::vector<IndexedNormTileWithColorSet> indexedNormTilesWithColorSets;
    std::unordered_set<ColorSet> colorSets;
    for (const auto& [index, normalizedTile] : indexedNormalizedTiles) {
        // Compute the ColorSet for this normalized tile, then add it to our indexes
        auto colorSet = toColorSet(colorIndexMap, normalizedTile.palette);
        indexedNormTilesWithColorSets.push_back({index, normalizedTile, colorSet});
        colorSets.insert(colorSet);
    }
    return std::pair{indexedNormTilesWithColorSets, colorSets};
}

CompiledTileset compile(const Config& config, const DecompiledTileset& decompiledTileset) {
    CompiledTileset compiled;
    // TODO : this needs to take into account secondary tilesets, so `numPalettesTotal - numPalettesInPrimary'
    compiled.palettes.resize(config.numPalettesInPrimary);
    compiled.assignments.resize(decompiledTileset.tiles.size());

    std::vector<IndexedNormTile> indexedNormTiles = normalizeDecompTiles(config, decompiledTileset);
    std::unordered_map<BGR15, std::size_t> colorIndexMap = buildColorIndexMap(config, indexedNormTiles);
    auto [indexedNormTilesWithColorSets, colorSets] = matchNormalizedWithColorSets(colorIndexMap, indexedNormTiles);

    return compiled;
}

}

TEST_CASE("insertRGBA should add new colors in order and return the correct index for a given color") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    porytiles::NormalizedPalette palette1{};
    palette1.size = 1;
    palette1.colors = {};

    // invalid alpha value, must be opaque or transparent
    CHECK_THROWS_WITH_AS(insertRGBA(config, palette1, porytiles::RGBA32{0, 0, 0, 12}),
                         "invalid alpha value: 12",
                         const porytiles::PtException&);

    // Transparent should return 0
    CHECK(insertRGBA(config, palette1, porytiles::RGBA_MAGENTA) == 0);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_TRANSPARENT}) == 0);

    // insert colors
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_OPAQUE}) == 1);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{8, 0, 0, porytiles::ALPHA_OPAQUE}) == 2);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{16, 0, 0, porytiles::ALPHA_OPAQUE}) == 3);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{24, 0, 0, porytiles::ALPHA_OPAQUE}) == 4);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{32, 0, 0, porytiles::ALPHA_OPAQUE}) == 5);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{40, 0, 0, porytiles::ALPHA_OPAQUE}) == 6);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{48, 0, 0, porytiles::ALPHA_OPAQUE}) == 7);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{56, 0, 0, porytiles::ALPHA_OPAQUE}) == 8);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{64, 0, 0, porytiles::ALPHA_OPAQUE}) == 9);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{72, 0, 0, porytiles::ALPHA_OPAQUE}) == 10);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{80, 0, 0, porytiles::ALPHA_OPAQUE}) == 11);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{88, 0, 0, porytiles::ALPHA_OPAQUE}) == 12);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{96, 0, 0, porytiles::ALPHA_OPAQUE}) == 13);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{104, 0, 0, porytiles::ALPHA_OPAQUE}) == 14);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{112, 0, 0, porytiles::ALPHA_OPAQUE}) == 15);

    // repeat colors should return their indexes
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{72, 0, 0, porytiles::ALPHA_OPAQUE}) == 10);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{112, 0, 0, porytiles::ALPHA_OPAQUE}) == 15);

    // Transparent should still return 0
    CHECK(insertRGBA(config, palette1, porytiles::RGBA_MAGENTA) == 0);
    CHECK(insertRGBA(config, palette1, porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_TRANSPARENT}) == 0);

    // Should throw, palette full
    CHECK_THROWS_WITH_AS(insertRGBA(config, palette1, porytiles::RGBA_CYAN),
                         "too many unique colors in tile",
                         const porytiles::PtException&);
}

TEST_CASE("candidate should return the NormalizedTile with requested flips") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    REQUIRE(std::filesystem::exists("res/tests/corners.png"));
    png::image<png::rgba_pixel> png1{"res/tests/corners.png"};
    porytiles::DecompiledTileset tiles = porytiles::importTilesFrom(png1);
    porytiles::RGBATile tile = tiles.tiles[0];

    SUBCASE("case: no flips") {
        porytiles::NormalizedTile candidate = porytiles::candidate(config, tile, false, false);
        CHECK(candidate.palette.size == 9);
        CHECK(candidate.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
        CHECK(candidate.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
        CHECK(candidate.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));
        CHECK(candidate.palette.colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
        CHECK(candidate.palette.colors[4] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
        CHECK(candidate.palette.colors[5] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
        CHECK(candidate.palette.colors[6] == porytiles::rgbaToBgr(porytiles::RGBA_BLACK));
        CHECK(candidate.palette.colors[7] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
        CHECK(candidate.palette.colors[8] == porytiles::rgbaToBgr(porytiles::RGBA_GREY));
        CHECK(candidate.pixels.paletteIndexes[0] == 1);
        CHECK(candidate.pixels.paletteIndexes[7] == 2);
        CHECK(candidate.pixels.paletteIndexes[9] == 3);
        CHECK(candidate.pixels.paletteIndexes[14] == 4);
        CHECK(candidate.pixels.paletteIndexes[18] == 2);
        CHECK(candidate.pixels.paletteIndexes[21] == 5);
        CHECK(candidate.pixels.paletteIndexes[42] == 3);
        CHECK(candidate.pixels.paletteIndexes[45] == 1);
        CHECK(candidate.pixels.paletteIndexes[49] == 6);
        CHECK(candidate.pixels.paletteIndexes[54] == 7);
        CHECK(candidate.pixels.paletteIndexes[56] == 8);
        CHECK(candidate.pixels.paletteIndexes[63] == 5);
    }

    SUBCASE("case: hFlip") {
        porytiles::NormalizedTile candidate = porytiles::candidate(config, tile, true, false);
        CHECK(candidate.palette.size == 9);
        CHECK(candidate.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
        CHECK(candidate.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));
        CHECK(candidate.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
        CHECK(candidate.palette.colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
        CHECK(candidate.palette.colors[4] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
        CHECK(candidate.palette.colors[5] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
        CHECK(candidate.palette.colors[6] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
        CHECK(candidate.palette.colors[7] == porytiles::rgbaToBgr(porytiles::RGBA_BLACK));
        CHECK(candidate.palette.colors[8] == porytiles::rgbaToBgr(porytiles::RGBA_GREY));
        CHECK(candidate.pixels.paletteIndexes[0] == 1);
        CHECK(candidate.pixels.paletteIndexes[7] == 2);
        CHECK(candidate.pixels.paletteIndexes[9] == 3);
        CHECK(candidate.pixels.paletteIndexes[14] == 4);
        CHECK(candidate.pixels.paletteIndexes[18] == 5);
        CHECK(candidate.pixels.paletteIndexes[21] == 1);
        CHECK(candidate.pixels.paletteIndexes[42] == 2);
        CHECK(candidate.pixels.paletteIndexes[45] == 4);
        CHECK(candidate.pixels.paletteIndexes[49] == 6);
        CHECK(candidate.pixels.paletteIndexes[54] == 7);
        CHECK(candidate.pixels.paletteIndexes[56] == 5);
        CHECK(candidate.pixels.paletteIndexes[63] == 8);
    }

    SUBCASE("case: vFlip") {
        porytiles::NormalizedTile candidate = porytiles::candidate(config, tile, false, true);
        CHECK(candidate.palette.size == 9);
        CHECK(candidate.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
        CHECK(candidate.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREY));
        CHECK(candidate.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
        CHECK(candidate.palette.colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_BLACK));
        CHECK(candidate.palette.colors[4] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
        CHECK(candidate.palette.colors[5] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
        CHECK(candidate.palette.colors[6] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
        CHECK(candidate.palette.colors[7] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));
        CHECK(candidate.palette.colors[8] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
        CHECK(candidate.pixels.paletteIndexes[0] == 1);
        CHECK(candidate.pixels.paletteIndexes[7] == 2);
        CHECK(candidate.pixels.paletteIndexes[9] == 3);
        CHECK(candidate.pixels.paletteIndexes[14] == 4);
        CHECK(candidate.pixels.paletteIndexes[18] == 5);
        CHECK(candidate.pixels.paletteIndexes[21] == 6);
        CHECK(candidate.pixels.paletteIndexes[42] == 7);
        CHECK(candidate.pixels.paletteIndexes[45] == 2);
        CHECK(candidate.pixels.paletteIndexes[49] == 5);
        CHECK(candidate.pixels.paletteIndexes[54] == 8);
        CHECK(candidate.pixels.paletteIndexes[56] == 6);
        CHECK(candidate.pixels.paletteIndexes[63] == 7);
    }

    SUBCASE("case: hFlip and vFlip") {
        porytiles::NormalizedTile candidate = porytiles::candidate(config, tile, true, true);
        CHECK(candidate.palette.size == 9);
        CHECK(candidate.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
        CHECK(candidate.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
        CHECK(candidate.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_GREY));
        CHECK(candidate.palette.colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
        CHECK(candidate.palette.colors[4] == porytiles::rgbaToBgr(porytiles::RGBA_BLACK));
        CHECK(candidate.palette.colors[5] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
        CHECK(candidate.palette.colors[6] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
        CHECK(candidate.palette.colors[7] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));
        CHECK(candidate.palette.colors[8] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
        CHECK(candidate.pixels.paletteIndexes[0] == 1);
        CHECK(candidate.pixels.paletteIndexes[7] == 2);
        CHECK(candidate.pixels.paletteIndexes[9] == 3);
        CHECK(candidate.pixels.paletteIndexes[14] == 4);
        CHECK(candidate.pixels.paletteIndexes[18] == 5);
        CHECK(candidate.pixels.paletteIndexes[21] == 6);
        CHECK(candidate.pixels.paletteIndexes[42] == 1);
        CHECK(candidate.pixels.paletteIndexes[45] == 7);
        CHECK(candidate.pixels.paletteIndexes[49] == 8);
        CHECK(candidate.pixels.paletteIndexes[54] == 6);
        CHECK(candidate.pixels.paletteIndexes[56] == 7);
        CHECK(candidate.pixels.paletteIndexes[63] == 5);
    }
}

TEST_CASE("normalize should return the normal form of the given tile") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    REQUIRE(std::filesystem::exists("res/tests/corners.png"));
    png::image<png::rgba_pixel> png1{"res/tests/corners.png"};
    porytiles::DecompiledTileset tiles = porytiles::importTilesFrom(png1);
    porytiles::RGBATile tile = tiles.tiles[0];

    porytiles::NormalizedTile normalizedTile = porytiles::normalize(config, tile);
    CHECK(normalizedTile.palette.size == 9);
    CHECK_FALSE(normalizedTile.hFlip);
    CHECK_FALSE(normalizedTile.vFlip);
    CHECK(normalizedTile.pixels.paletteIndexes[0] == 1);
    CHECK(normalizedTile.pixels.paletteIndexes[7] == 2);
    CHECK(normalizedTile.pixels.paletteIndexes[9] == 3);
    CHECK(normalizedTile.pixels.paletteIndexes[14] == 4);
    CHECK(normalizedTile.pixels.paletteIndexes[18] == 2);
    CHECK(normalizedTile.pixels.paletteIndexes[21] == 5);
    CHECK(normalizedTile.pixels.paletteIndexes[42] == 3);
    CHECK(normalizedTile.pixels.paletteIndexes[45] == 1);
    CHECK(normalizedTile.pixels.paletteIndexes[49] == 6);
    CHECK(normalizedTile.pixels.paletteIndexes[54] == 7);
    CHECK(normalizedTile.pixels.paletteIndexes[56] == 8);
    CHECK(normalizedTile.pixels.paletteIndexes[63] == 5);
}

TEST_CASE("normalizeDecompTiles should correctly normalize all tiles in the decomp tileset") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
    png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
    porytiles::DecompiledTileset tiles = porytiles::importTilesFrom(png1);

    std::vector<IndexedNormTile> indexedNormTiles = normalizeDecompTiles(config, tiles);

    CHECK(indexedNormTiles.size() == 4);

    // First tile normal form is vFlipped, palette should have 2 colors
    CHECK(indexedNormTiles[0].second.pixels.paletteIndexes[0] == 0);
    CHECK(indexedNormTiles[0].second.pixels.paletteIndexes[7] == 1);
    for (int i = 56; i <= 63; i++) {
        CHECK(indexedNormTiles[0].second.pixels.paletteIndexes[i] == 1);
    }
    CHECK(indexedNormTiles[0].second.palette.size == 2);
    CHECK(indexedNormTiles[0].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(indexedNormTiles[0].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
    CHECK_FALSE(indexedNormTiles[0].second.hFlip);
    CHECK(indexedNormTiles[0].second.vFlip);
    CHECK(indexedNormTiles[0].first == 0);

    // Second tile already in normal form, palette should have 3 colors
    CHECK(indexedNormTiles[1].second.pixels.paletteIndexes[0] == 0);
    CHECK(indexedNormTiles[1].second.pixels.paletteIndexes[54] == 1);
    CHECK(indexedNormTiles[1].second.pixels.paletteIndexes[55] == 1);
    CHECK(indexedNormTiles[1].second.pixels.paletteIndexes[62] == 1);
    CHECK(indexedNormTiles[1].second.pixels.paletteIndexes[63] == 2);
    CHECK(indexedNormTiles[1].second.palette.size == 3);
    CHECK(indexedNormTiles[1].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(indexedNormTiles[1].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
    CHECK(indexedNormTiles[1].second.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
    CHECK_FALSE(indexedNormTiles[1].second.hFlip);
    CHECK_FALSE(indexedNormTiles[1].second.vFlip);
    CHECK(indexedNormTiles[1].first == 1);

    // Third tile normal form is hFlipped, palette should have 3 colors
    CHECK(indexedNormTiles[2].second.pixels.paletteIndexes[0] == 0);
    CHECK(indexedNormTiles[2].second.pixels.paletteIndexes[7] == 1);
    CHECK(indexedNormTiles[2].second.pixels.paletteIndexes[56] == 1);
    CHECK(indexedNormTiles[2].second.pixels.paletteIndexes[63] == 2);
    CHECK(indexedNormTiles[2].second.palette.size == 3);
    CHECK(indexedNormTiles[2].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(indexedNormTiles[2].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
    CHECK(indexedNormTiles[2].second.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
    CHECK_FALSE(indexedNormTiles[2].second.vFlip);
    CHECK(indexedNormTiles[2].second.hFlip);
    CHECK(indexedNormTiles[2].first == 2);

    // Fourth tile normal form is hFlipped and vFlipped, palette should have 2 colors
    CHECK(indexedNormTiles[3].second.pixels.paletteIndexes[0] == 0);
    CHECK(indexedNormTiles[3].second.pixels.paletteIndexes[7] == 1);
    for (int i = 56; i <= 63; i++) {
        CHECK(indexedNormTiles[3].second.pixels.paletteIndexes[i] == 1);
    }
    CHECK(indexedNormTiles[3].second.palette.size == 2);
    CHECK(indexedNormTiles[3].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(indexedNormTiles[3].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
    CHECK(indexedNormTiles[3].second.hFlip);
    CHECK(indexedNormTiles[3].second.vFlip);
    CHECK(indexedNormTiles[3].first == 3);
}

TEST_CASE("buildColorIndexMap should build a map of all unique colors in the decomp tileset") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
    png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
    porytiles::DecompiledTileset tiles = porytiles::importTilesFrom(png1);
    std::vector<IndexedNormTile> normalizedTiles = normalizeDecompTiles(config, tiles);

    std::unordered_map<porytiles::BGR15, std::size_t> colorIndexMap = porytiles::buildColorIndexMap(config,
                                                                                                    normalizedTiles);

    CHECK(colorIndexMap.size() == 4);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 0);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 1);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 2);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_CYAN)] == 3);
}

TEST_CASE("toColorSet should return the correct bitset based on the supplied palette") {
    std::unordered_map<porytiles::BGR15, std::size_t> colorIndexMap = {
            {porytiles::rgbaToBgr(porytiles::RGBA_BLUE),   0},
            {porytiles::rgbaToBgr(porytiles::RGBA_RED),    1},
            {porytiles::rgbaToBgr(porytiles::RGBA_GREEN),  2},
            {porytiles::rgbaToBgr(porytiles::RGBA_CYAN),   3},
            {porytiles::rgbaToBgr(porytiles::RGBA_YELLOW), 4},
    };

    SUBCASE("palette 1") {
        porytiles::NormalizedPalette palette{};
        palette.size = 2;
        palette.colors[0] = porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA);
        palette.colors[1] = porytiles::rgbaToBgr(porytiles::RGBA_RED);

        ColorSet colorSet = porytiles::toColorSet(colorIndexMap, palette);
        CHECK(colorSet.count() == 1);
        CHECK(colorSet.test(1));
    }

    SUBCASE("palette 2") {
        porytiles::NormalizedPalette palette{};
        palette.size = 4;
        palette.colors[0] = porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA);
        palette.colors[1] = porytiles::rgbaToBgr(porytiles::RGBA_YELLOW);
        palette.colors[2] = porytiles::rgbaToBgr(porytiles::RGBA_GREEN);
        palette.colors[3] = porytiles::rgbaToBgr(porytiles::RGBA_CYAN);

        ColorSet colorSet = porytiles::toColorSet(colorIndexMap, palette);
        CHECK(colorSet.count() == 3);
        CHECK(colorSet.test(4));
        CHECK(colorSet.test(2));
        CHECK(colorSet.test(3));
    }
}

TEST_CASE("matchNormalizedWithColorSets should return the expected data structures") {
    porytiles::Config config{};
    config.transparencyColor = porytiles::RGBA_MAGENTA;
    config.numPalettesInPrimary = 6;

    REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
    png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
    porytiles::DecompiledTileset tiles = porytiles::importTilesFrom(png1);
    std::vector<IndexedNormTile> indexedNormTiles = normalizeDecompTiles(config, tiles);
    std::unordered_map<porytiles::BGR15, std::size_t> colorIndexMap = porytiles::buildColorIndexMap(config,
                                                                                                    indexedNormTiles);
                                                                                                    CHECK(colorIndexMap.size() == 4);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 0);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 1);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 2);
    CHECK(colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_CYAN)] == 3);

    auto [indexedNormTilesWithColorSets, colorSets] = matchNormalizedWithColorSets(colorIndexMap, indexedNormTiles);

    CHECK(indexedNormTilesWithColorSets.size() == 4);
    // colorSets size is 3 because first and fourth tiles have the same palette
    CHECK(colorSets.size() == 3);

    // First tile has 1 non-transparent color, color should be BLUE
    CHECK(std::get<0>(indexedNormTilesWithColorSets[0]) == 0);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.paletteIndexes[0] == 0);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.paletteIndexes[7] == 1);
    for (int i = 56; i <= 63; i++) {
        CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.paletteIndexes[i] == 1);
    }
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.size == 2);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
    CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[0]).hFlip);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).vFlip);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[0]).count() == 1);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[0]).test(0));
    CHECK(colorSets.contains(std::get<2>(indexedNormTilesWithColorSets[0])));

    // Second tile has two non-transparent colors, RED and GREEN
    CHECK(std::get<0>(indexedNormTilesWithColorSets[1]) == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.paletteIndexes[0] == 0);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.paletteIndexes[54] == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.paletteIndexes[55] == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.paletteIndexes[62] == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.paletteIndexes[63] == 2);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.size == 3);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
    CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[1]).hFlip);
    CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[1]).vFlip);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).count() == 2);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).test(1));
    CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).test(2));
    CHECK(colorSets.contains(std::get<2>(indexedNormTilesWithColorSets[1])));

    // Third tile has two non-transparent colors, CYAN and GREEN
    CHECK(std::get<0>(indexedNormTilesWithColorSets[2]) == 2);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.paletteIndexes[0] == 0);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.paletteIndexes[7] == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.paletteIndexes[56] == 1);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.paletteIndexes[63] == 2);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.size == 3);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
    CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[2]).vFlip);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).hFlip);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).count() == 2);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).test(1));
    CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).test(3));
    CHECK(colorSets.contains(std::get<2>(indexedNormTilesWithColorSets[2])));

    // Fourth tile has 1 non-transparent color, color should be BLUE
    CHECK(std::get<0>(indexedNormTilesWithColorSets[3]) == 3);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.paletteIndexes[0] == 0);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.paletteIndexes[7] == 1);
    for (int i = 56; i <= 63; i++) {
        CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.paletteIndexes[i] == 1);
    }
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.size == 2);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).hFlip);
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).vFlip);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[3]).count() == 1);
    CHECK(std::get<2>(indexedNormTilesWithColorSets[3]).test(0));
    CHECK(colorSets.contains(std::get<2>(indexedNormTilesWithColorSets[3])));
}
