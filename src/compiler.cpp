#include "compiler.h"

#include <algorithm>
#include <bitset>
#include <doctest.h>
#include <memory>
#include <png.hpp>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "errors.h"
#include "importer.h"
#include "logger.h"
#include "ptcontext.h"
#include "ptexception.h"
#include "types.h"

namespace porytiles {
// TODO : change this to receive CompilerContext once I have made that type available
size_t insertRGBA(const RGBA32 &transparencyColor, NormalizedPalette &palette, RGBA32 rgba)
{
  /*
   * Insert an rgba32 color into a normalized palette. The color will be converted to bgr15 format in the process,
   * and possibly deduped (depending on user settings). Transparent alpha pixels will be treated as transparent, as
   * will pixels that are of transparent color (again, set by the user but default to magenta). Fails if a tile
   * contains too many unique colors or if an invalid alpha value is detected.
   */
  if (rgba.alpha == ALPHA_TRANSPARENT || rgba == transparencyColor) {
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
      palette.colors.at(palette.size++) = bgr;
    }
    return bgrPosInPalette;
  }
  // TODO : better error context
  throw PtException{"invalid alpha value: " + std::to_string(rgba.alpha)};
}

NormalizedTile candidate(const RGBA32 &transparencyColor, const RGBATile &rgba, bool hFlip, bool vFlip)
{
  /*
   * NOTE: This only produces a _candidate_ normalized tile (a different choice of hFlip/vFlip might be the normal
   * form). We'll use this to generate candidates to find the true normal form.
   */
  // TODO : same color precision note as above in insertRGBA
  NormalizedTile candidateTile{transparencyColor};
  candidateTile.hFlip = hFlip;
  candidateTile.vFlip = vFlip;

  for (std::size_t row = 0; row < TILE_SIDE_LENGTH; row++) {
    for (std::size_t col = 0; col < TILE_SIDE_LENGTH; col++) {
      std::size_t rowWithFlip = vFlip ? TILE_SIDE_LENGTH - 1 - row : row;
      std::size_t colWithFlip = hFlip ? TILE_SIDE_LENGTH - 1 - col : col;
      candidateTile.setPixel(
          row, col, insertRGBA(transparencyColor, candidateTile.palette, rgba.getPixel(rowWithFlip, colWithFlip)));
    }
  }

  return candidateTile;
}

NormalizedTile normalize(const RGBA32 &transparencyColor, const RGBATile &rgba)
{
  /*
   * Normalize the given tile by checking each of the 4 possible flip states, and choosing the one that comes first in
   * "lexicographic" order, where this order is determined by the std::array spaceship operator.
   */
  auto noFlipsTile = candidate(transparencyColor, rgba, false, false);

  // Short-circuit because transparent tiles are common in metatiles and trivially in normal form.
  if (noFlipsTile.transparent()) {
    return noFlipsTile;
  }

  auto hFlipTile = candidate(transparencyColor, rgba, true, false);
  auto vFlipTile = candidate(transparencyColor, rgba, false, true);
  auto bothFlipsTile = candidate(transparencyColor, rgba, true, true);

  std::array<const NormalizedTile *, 4> candidates = {&noFlipsTile, &hFlipTile, &vFlipTile, &bothFlipsTile};
  auto normalizedTile = std::min_element(std::begin(candidates), std::end(candidates),
                                         [](auto tile1, auto tile2) { return tile1->pixels < tile2->pixels; });
  return **normalizedTile;
}

std::vector<IndexedNormTile> normalizeDecompTiles(const RGBA32 &transparencyColor,
                                                  const DecompiledTileset &decompiledTileset)
{
  /*
   * For each tile in the decomp tileset, normalize it and tag it with its index in the decomp tileset.
   */
  std::vector<IndexedNormTile> normalizedTiles;
  DecompiledIndex decompiledIndex = 0;
  for (auto const &tile : decompiledTileset.tiles) {
    auto normalizedTile = normalize(transparencyColor, tile);
    normalizedTiles.emplace_back(decompiledIndex++, normalizedTile);
  }
  return normalizedTiles;
}

std::pair<std::unordered_map<BGR15, std::size_t>, std::unordered_map<std::size_t, BGR15>>
buildColorIndexMaps(PtContext &ctx, const std::vector<IndexedNormTile> &normalizedTiles,
                    const std::unordered_map<BGR15, std::size_t> &primaryIndexMap)
{
  /*
   * Iterate over every color in each tile's NormalizedPalette, adding it to the map if not already present. We end up
   * with a map of colors to unique indexes. Optionally, we will populate the map with colors from the paired primary
   * set so that secondary tiles can possibly make use of these palettes without doubling up colors.
   */
  std::unordered_map<BGR15, std::size_t> colorIndexes;
  std::unordered_map<std::size_t, BGR15> indexesToColors;
  if (!primaryIndexMap.empty()) {
    for (const auto &[color, index] : primaryIndexMap) {
      auto [insertedValue, wasInserted] = colorIndexes.insert({color, index});
      if (!wasInserted) {
        // TODO : better error context
        throw std::runtime_error{"double inserted a color from primary set"};
      }
      indexesToColors.insert(std::pair{index, color});
      // TODO : throw here if insert fails?
    }
  }
  std::size_t colorIndex = primaryIndexMap.size();
  for (const auto &[_, normalizedTile] : normalizedTiles) {
    // i starts at 1, since first color in each palette is the transparency color
    for (int i = 1; i < normalizedTile.palette.size; i++) {
      const BGR15 &color = normalizedTile.palette.colors[i];
      bool inserted = colorIndexes.insert(std::pair{color, colorIndex}).second;
      if (inserted) {
        indexesToColors.insert(std::pair{colorIndex, color});
        colorIndex++;
      }
    }
  }

  if (colorIndex > (PAL_SIZE - 1) * ctx.fieldmapConfig.numPalettesInPrimary) {
    // TODO : better error context
    throw PtException{"too many unique colors"};
  }

  return {colorIndexes, indexesToColors};
}

ColorSet toColorSet(const std::unordered_map<BGR15, std::size_t> &colorIndexMap, const NormalizedPalette &palette)
{
  /*
   * Set a color set based on a given palette. Each bit in the ColorSet represents if the color at the given index in
   * the supplied color map was present in the palette. E.g. suppose the color map has 12 unique colors. The supplied
   * palette has two colors in it, which correspond to index 2 and index 11. The ColorSet bitset would be:
   * 0010 0000 0001
   */
  ColorSet colorSet;
  // starts at 1, skip the transparent color at slot 0 in the normalized palette
  for (int i = 1; i < palette.size; i++) {
    colorSet.set(colorIndexMap.at(palette.colors.at(i)));
  }
  return colorSet;
}

std::pair<std::vector<IndexedNormTileWithColorSet>, std::vector<ColorSet>>
matchNormalizedWithColorSets(const std::unordered_map<BGR15, std::size_t> &colorIndexMap,
                             const std::vector<IndexedNormTile> &indexedNormalizedTiles)
{
  std::vector<IndexedNormTileWithColorSet> indexedNormTilesWithColorSets;
  std::unordered_set<ColorSet> uniqueColorSets;
  std::vector<ColorSet> colorSets;
  for (const auto &[index, normalizedTile] : indexedNormalizedTiles) {
    // Compute the ColorSet for this normalized tile, then add it to our indexes
    auto colorSet = toColorSet(colorIndexMap, normalizedTile.palette);
    indexedNormTilesWithColorSets.emplace_back(index, normalizedTile, colorSet);
    if (!uniqueColorSets.contains(colorSet)) {
      colorSets.push_back(colorSet);
      uniqueColorSets.insert(colorSet);
    }
  }
  return std::pair{indexedNormTilesWithColorSets, colorSets};
}

struct AssignState {
  /*
   * One color set for each hardware palette, bits in color set will indicate which colors this HW palette will have.
   * The size of the vector should be fixed to maxPalettes.
   */
  std::vector<ColorSet> hardwarePalettes;

  // The unique color sets from the NormalizedTiles
  std::vector<ColorSet> unassigned;
};
std::size_t gRecurseCount = 0;
bool assign(const std::size_t maxRecurseCount, AssignState state, std::vector<ColorSet> &solution,
            const std::vector<ColorSet> &primaryPalettes)
{
  gRecurseCount++;
  // TODO : this is a horrible hack avert your eyes
  if (gRecurseCount > maxRecurseCount) {
    // TODO : better error context
    throw PtException{"too many assignment recurses"};
  }

  if (state.unassigned.empty()) {
    // No tiles left to assign, found a solution!
    std::copy(std::begin(state.hardwarePalettes), std::end(state.hardwarePalettes), std::back_inserter(solution));
    return true;
  }

  /*
   * We will try to assign the last element to one of the 6 hw palettes, last because it is a vector so easier to
   * add/remove from the end.
   */
  ColorSet &toAssign = state.unassigned.back();

  /*
   * If we are assigning a secondary set, we'll want to first check if any of the primary palettes satisfy the color
   * constraints for this particular tile. That way we can just use the primary palette, since those are available for
   * secondary tiles to freely use.
   */
  if (!primaryPalettes.empty()) {
    for (size_t i = 0; i < primaryPalettes.size(); i++) {
      const ColorSet &palette = primaryPalettes.at(i);
      if ((palette | toAssign).count() == palette.count()) {
        /*
         * This case triggers if `toAssign' shares all its colors with one of the palettes from the primary
         * tileset. In that case, we will just reuse that palette when we make the tile in a later step. So we
         * can prep a recursive call to assign with an unchanged state (other than removing `toAssign')
         */
        std::vector<ColorSet> unassignedCopy;
        std::copy(std::begin(state.unassigned), std::end(state.unassigned), std::back_inserter(unassignedCopy));
        std::vector<ColorSet> hardwarePalettesCopy;
        std::copy(std::begin(state.hardwarePalettes), std::end(state.hardwarePalettes),
                  std::back_inserter(hardwarePalettesCopy));
        unassignedCopy.pop_back();
        AssignState updatedState = {hardwarePalettesCopy, unassignedCopy};

        if (assign(maxRecurseCount, updatedState, solution, primaryPalettes)) {
          return true;
        }
      }
    }
  }

  /*
   * For this next step, we want to sort the hw palettes before we try iterating. Sort them by the size of their
   * intersection with the toAssign ColorSet. Effectively, this means that we will always first try branching into an
   * assignment that re-uses hw palettes more effectively. We also have a tie-breaker heuristic for cases where two
   * palettes have the same intersect size. Right now we just use palette size, but in the future we may want to look
   * at color distances so we can pick a palette with more similar colors.
   */
  std::stable_sort(std::begin(state.hardwarePalettes), std::end(state.hardwarePalettes),
                   [&toAssign](const auto &pal1, const auto &pal2) {
                     std::size_t pal1IntersectSize = (pal1 & toAssign).count();
                     std::size_t pal2IntersectSize = (pal2 & toAssign).count();

                     /*
                      * TODO : Instead of just using palette count, maybe can we check for color distance here and try
                      * to choose the palette that has the "closest" colors to our toAssign palette? That might be a
                      * good heuristic for attempting to keep similar colors in the same palette. I.e. especially in
                      * cases where there are no palette intersections, it may be better to first try placing the new
                      * colors into a palette with similar colors rather than into the smallest palette
                      */
                     if (pal1IntersectSize == pal2IntersectSize) {
                       return pal1.count() < pal2.count();
                     }

                     return pal1IntersectSize > pal2IntersectSize;
                   });

  for (size_t i = 0; i < state.hardwarePalettes.size(); i++) {
    const ColorSet &palette = state.hardwarePalettes.at(i);

    // > PAL_SIZE - 1 because we need to save a slot for transparency
    if ((palette | toAssign).count() > PAL_SIZE - 1) {
      /*
       *  Skip this palette, cannot assign because there is not enough room in the palette. If we end up skipping
       * all of them that means the palettes are all too full and we cannot assign this tile in the state we are
       * in. The algorithm will be forced to backtrack and try other assignments.
       */
      continue;
    }

    /*
     * Prep the recursive call to assign(). If we got here, we know it is possible to assign toAssign to the palette
     * at hardwarePalettes[i]. So we make a copy of unassigned with toAssign removed and a copy of hardwarePalettes
     * with toAssigned assigned to the palette at index i. Then we call assign again with this updated state, and
     * return true if there is a valid solution somewhere down in this recursive branch.
     */
    std::vector<ColorSet> unassignedCopy;
    std::copy(std::begin(state.unassigned), std::end(state.unassigned), std::back_inserter(unassignedCopy));
    std::vector<ColorSet> hardwarePalettesCopy;
    std::copy(std::begin(state.hardwarePalettes), std::end(state.hardwarePalettes),
              std::back_inserter(hardwarePalettesCopy));
    unassignedCopy.pop_back();
    hardwarePalettesCopy.at(i) |= toAssign;
    AssignState updatedState = {hardwarePalettesCopy, unassignedCopy};

    if (assign(maxRecurseCount, updatedState, solution, primaryPalettes)) {
      return true;
    }
  }

  // No solution found
  return false;
}

GBATile makeTile(const NormalizedTile &normalizedTile, GBAPalette palette)
{
  GBATile gbaTile{};
  std::array<std::uint8_t, PAL_SIZE> paletteIndexes{};
  paletteIndexes.at(0) = 0;
  for (int i = 1; i < normalizedTile.palette.size; i++) {
    auto it = std::find(std::begin(palette.colors) + 1, std::end(palette.colors), normalizedTile.palette.colors[i]);
    if (it == std::end(palette.colors)) {
      // TODO : better error context
      throw std::runtime_error{"it == std::end(palette.colors)"};
    }
    paletteIndexes.at(i) = it - std::begin(palette.colors);
  }
  for (std::size_t i = 0; i < normalizedTile.pixels.colorIndexes.size(); i++) {
    gbaTile.colorIndexes.at(i) = paletteIndexes.at(normalizedTile.pixels.colorIndexes.at(i));
  }
  return gbaTile;
}

void assignTilesPrimary(PtContext &ctx, CompiledTileset &compiled,
                        const std::vector<IndexedNormTileWithColorSet> &indexedNormTilesWithColorSets,
                        const std::vector<ColorSet> &assignedPalsSolution)
{
  std::unordered_map<GBATile, std::size_t> tileIndexes;
  // force tile 0 to be a transparent tile that uses palette 0
  tileIndexes.insert({GBA_TILE_TRANSPARENT, 0});
  compiled.tiles.push_back(GBA_TILE_TRANSPARENT);
  compiled.paletteIndexesOfTile.push_back(0);
  for (const auto &indexedNormTile : indexedNormTilesWithColorSets) {
    auto index = std::get<0>(indexedNormTile);
    auto &normTile = std::get<1>(indexedNormTile);
    auto &colorSet = std::get<2>(indexedNormTile);
    auto it = std::find_if(std::begin(assignedPalsSolution), std::end(assignedPalsSolution),
                           [&colorSet](const auto &assignedPal) {
                             // Find which of the assignedSolution palettes this tile belongs to
                             return (colorSet & ~assignedPal).none();
                           });
    if (it == std::end(assignedPalsSolution)) {
      // TODO : better error context
      throw std::runtime_error{"it == std::end(assignedPalsSolution)"};
    }
    std::size_t paletteIndex = it - std::begin(assignedPalsSolution);
    GBATile gbaTile = makeTile(normTile, compiled.palettes[paletteIndex]);
    // insert only updates the map if the key is not already present
    auto inserted = tileIndexes.insert({gbaTile, compiled.tiles.size()});
    if (inserted.second) {
      compiled.tiles.push_back(gbaTile);
      if (compiled.tiles.size() > ctx.fieldmapConfig.numTilesInPrimary) {
        // TODO : better error context
        throw PtException{"too many tiles: " + std::to_string(compiled.tiles.size()) + " > " +
                          std::to_string(ctx.fieldmapConfig.numTilesInPrimary)};
      }
      compiled.paletteIndexesOfTile.push_back(paletteIndex);
    }
    std::size_t tileIndex = inserted.first->second;
    compiled.assignments.at(index) = {tileIndex, paletteIndex, normTile.hFlip, normTile.vFlip};
  }
  compiled.tileIndexes = tileIndexes;
}

void assignTilesSecondary(PtContext &ctx, CompiledTileset &compiled,
                          const std::vector<IndexedNormTileWithColorSet> &indexedNormTilesWithColorSets,
                          const std::vector<ColorSet> &primaryPaletteColorSets,
                          const std::vector<ColorSet> &assignedPalsSolution)
{
  std::vector<ColorSet> allColorSets{};
  allColorSets.insert(allColorSets.end(), primaryPaletteColorSets.begin(), primaryPaletteColorSets.end());
  allColorSets.insert(allColorSets.end(), assignedPalsSolution.begin(), assignedPalsSolution.end());
  std::unordered_map<GBATile, std::size_t> tileIndexes;
  for (const auto &indexedNormTile : indexedNormTilesWithColorSets) {
    auto index = std::get<0>(indexedNormTile);
    auto &normTile = std::get<1>(indexedNormTile);
    auto &colorSet = std::get<2>(indexedNormTile);
    auto it = std::find_if(std::begin(allColorSets), std::end(allColorSets), [&colorSet](const auto &assignedPal) {
      // Find which of the allColorSets palettes this tile belongs to
      return (colorSet & ~assignedPal).none();
    });
    if (it == std::end(allColorSets)) {
      // TODO : better error context
      throw std::runtime_error{"it == std::end(allColorSets)"};
    }
    std::size_t paletteIndex = it - std::begin(allColorSets);
    GBATile gbaTile = makeTile(normTile, compiled.palettes[paletteIndex]);
    if (ctx.compilerContext.pairedPrimaryTiles->tileIndexes.find(gbaTile) !=
        ctx.compilerContext.pairedPrimaryTiles->tileIndexes.end()) {
      // Tile was in the primary set
      compiled.assignments.at(index) = {ctx.compilerContext.pairedPrimaryTiles->tileIndexes.at(gbaTile), paletteIndex,
                                        normTile.hFlip, normTile.vFlip};
    }
    else {
      // Tile was in the secondary set
      auto inserted = tileIndexes.insert({gbaTile, compiled.tiles.size()});
      if (inserted.second) {
        compiled.tiles.push_back(gbaTile);
        if (compiled.tiles.size() > ctx.fieldmapConfig.numTilesInSecondary()) {
          // TODO : better error context
          throw PtException{"too many tiles: " + std::to_string(compiled.tiles.size()) + " > " +
                            std::to_string(ctx.fieldmapConfig.numTilesInSecondary())};
        }
        compiled.paletteIndexesOfTile.push_back(paletteIndex);
      }
      std::size_t tileIndex = inserted.first->second;
      // Offset the tile index by the secondary tileset VRAM location, which is just the size of the primary tiles
      compiled.assignments.at(index) = {tileIndex + ctx.fieldmapConfig.numTilesInPrimary, paletteIndex, normTile.hFlip,
                                        normTile.vFlip};
    }
  }
  compiled.tileIndexes = tileIndexes;
}

std::unique_ptr<CompiledTileset> compile(PtContext &ctx, const DecompiledTileset &decompiledTileset)
{
  if (ctx.compilerConfig.mode == CompilerMode::SECONDARY &&
      (ctx.fieldmapConfig.numPalettesInPrimary != ctx.compilerContext.pairedPrimaryTiles->palettes.size())) {
    internalerror_numPalettesInPrimaryNeqPrimaryPalettesSize(ctx.fieldmapConfig.numPalettesInPrimary,
                                                             ctx.compilerContext.pairedPrimaryTiles->palettes.size());
  }

  auto compiled = std::make_unique<CompiledTileset>();

  if (ctx.compilerConfig.mode == CompilerMode::PRIMARY) {
    compiled->palettes.resize(ctx.fieldmapConfig.numPalettesInPrimary);
    std::size_t inputMetatileCount = (decompiledTileset.tiles.size() / ctx.fieldmapConfig.numTilesPerMetatile);
    if (inputMetatileCount > ctx.fieldmapConfig.numMetatilesInPrimary) {
      throw PtException{"input metatile count (" + std::to_string(inputMetatileCount) +
                        ") exceeded primary metatile limit (" +
                        std::to_string(ctx.fieldmapConfig.numMetatilesInPrimary) + ")"};
    }
  }
  else if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    compiled->palettes.resize(ctx.fieldmapConfig.numPalettesTotal);
    std::size_t inputMetatileCount = (decompiledTileset.tiles.size() / ctx.fieldmapConfig.numTilesPerMetatile);
    if (inputMetatileCount > ctx.fieldmapConfig.numMetatilesInSecondary()) {
      throw PtException{"input metatile count (" + std::to_string(inputMetatileCount) +
                        ") exceeded secondary metatile limit (" +
                        std::to_string(ctx.fieldmapConfig.numMetatilesInSecondary()) + ")"};
    }
  }
  else if (ctx.compilerConfig.mode == CompilerMode::FREESTANDING) {
    throw std::runtime_error{"TODO : support FREESTANDING mode"};
  }
  else {
    internalerror_unknownCompilerMode(ctx.compilerConfig.mode);
  }
  compiled->assignments.resize(decompiledTileset.tiles.size());

  /*
   * Build indexed normalized tiles, order of this vector matches the decompiled iteration order, with animated tiles
   * at the beginning
   */
  std::vector<IndexedNormTile> indexedNormTiles =
      normalizeDecompTiles(ctx.compilerConfig.transparencyColor, decompiledTileset);

  /*
   * Map each unique color to a unique index between 0 and 240 (15 colors per palette * 16 palettes MAX)
   */
  std::unordered_map<BGR15, std::size_t> emptyPrimaryColorIndexMap;
  const std::unordered_map<BGR15, std::size_t> *primaryColorIndexMap = &emptyPrimaryColorIndexMap;
  if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    primaryColorIndexMap = &(ctx.compilerContext.pairedPrimaryTiles->colorIndexMap);
  }
  auto [colorToIndex, indexToColor] = buildColorIndexMaps(ctx, indexedNormTiles, *primaryColorIndexMap);
  compiled->colorIndexMap = colorToIndex;

  /*
   * colorSets is a vector: this enforces a well-defined ordering so tileset compilation results are identical across
   * all compilers and platforms. A ColorSet is just a bitset<240> that marks which colors are present (indexes are
   * based on the colorIndexMaps from above)
   */
  auto [indexedNormTilesWithColorSets, colorSets] = matchNormalizedWithColorSets(colorToIndex, indexedNormTiles);

  /*
   * Run palette assignment:
   * `assignedPalsSolution' is an out param that the assign function will populate when it finds a solution
   */
  std::vector<ColorSet> assignedPalsSolution;
  std::vector<ColorSet> tmpHardwarePalettes;
  if (ctx.compilerConfig.mode == CompilerMode::PRIMARY) {
    assignedPalsSolution.reserve(ctx.fieldmapConfig.numPalettesInPrimary);
    tmpHardwarePalettes.resize(ctx.fieldmapConfig.numPalettesInPrimary);
  }
  else if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    assignedPalsSolution.reserve(ctx.fieldmapConfig.numPalettesInSecondary());
    tmpHardwarePalettes.resize(ctx.fieldmapConfig.numPalettesInSecondary());
  }
  else if (ctx.compilerConfig.mode == CompilerMode::FREESTANDING) {
    throw std::runtime_error{"TODO : support FREESTANDING mode"};
  }
  else {
    internalerror_unknownCompilerMode(ctx.compilerConfig.mode);
  }
  std::vector<ColorSet> unassignedNormPalettes;
  std::copy(std::begin(colorSets), std::end(colorSets), std::back_inserter(unassignedNormPalettes));
  std::stable_sort(std::begin(unassignedNormPalettes), std::end(unassignedNormPalettes),
                   [](const auto &cs1, const auto &cs2) { return cs1.count() < cs2.count(); });
  std::vector<ColorSet> primaryPaletteColorSets{};
  if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    /*
     * Construct ColorSets for the primary palettes, assign can use these to decide if a tile is entirely covered by a
     * primary palette and hence does not need to extend the search by assigning its colors to one of the new secondary
     * palettes.
     */
    primaryPaletteColorSets.reserve(ctx.compilerContext.pairedPrimaryTiles->palettes.size());
    for (std::size_t i = 0; i < ctx.compilerContext.pairedPrimaryTiles->palettes.size(); i++) {
      const auto &gbaPalette = ctx.compilerContext.pairedPrimaryTiles->palettes.at(i);
      primaryPaletteColorSets.emplace_back();
      for (std::size_t j = 1; j < gbaPalette.size; j++) {
        primaryPaletteColorSets.at(i).set(colorToIndex.at(gbaPalette.colors.at(j)));
      }
    }
  }

  AssignState state = {tmpHardwarePalettes, unassignedNormPalettes};
  gRecurseCount = 0;
  bool assignSuccessful =
      assign(ctx.compilerConfig.maxRecurseCount, state, assignedPalsSolution, primaryPaletteColorSets);
  if (!assignSuccessful) {
    // TODO : better error context
    throw PtException{"failed to allocate palettes"};
  }

  /*
   * Copy the assignments into the compiled palettes. In a future version we will support sibling tiles (tile sharing)
   * and so we may need to do something fancier here so that the colors align correctly.
   */
  if (ctx.compilerConfig.mode == CompilerMode::PRIMARY) {
    for (std::size_t i = 0; i < ctx.fieldmapConfig.numPalettesInPrimary; i++) {
      ColorSet palAssignments = assignedPalsSolution.at(i);
      compiled->palettes.at(i).colors.at(0) = rgbaToBgr(ctx.compilerConfig.transparencyColor);
      std::size_t colorIndex = 1;
      for (std::size_t j = 0; j < palAssignments.size(); j++) {
        if (palAssignments.test(j)) {
          compiled->palettes.at(i).colors.at(colorIndex) = indexToColor.at(j);
          colorIndex++;
        }
      }
      compiled->palettes.at(i).size = colorIndex;
    }
  }
  else if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    for (std::size_t i = 0; i < ctx.fieldmapConfig.numPalettesInPrimary; i++) {
      // Copy the primary set's palettes into this tileset so tiles can use them
      for (std::size_t j = 0; j < PAL_SIZE; j++) {
        compiled->palettes.at(i).colors.at(j) = ctx.compilerContext.pairedPrimaryTiles->palettes.at(i).colors.at(j);
      }
    }
    for (std::size_t i = ctx.fieldmapConfig.numPalettesInPrimary; i < ctx.fieldmapConfig.numPalettesTotal; i++) {
      ColorSet palAssignments = assignedPalsSolution.at(i - ctx.fieldmapConfig.numPalettesInPrimary);
      compiled->palettes.at(i).colors.at(0) = rgbaToBgr(ctx.compilerConfig.transparencyColor);
      std::size_t colorIndex = 1;
      for (std::size_t j = 0; j < palAssignments.size(); j++) {
        if (palAssignments.test(j)) {
          compiled->palettes.at(i).colors.at(colorIndex) = indexToColor.at(j);
          colorIndex++;
        }
      }
      compiled->palettes.at(i).size = colorIndex;
    }
  }
  else if (ctx.compilerConfig.mode == CompilerMode::FREESTANDING) {
    throw std::runtime_error{"TODO : support FREESTANDING mode"};
  }
  else {
    internalerror_unknownCompilerMode(ctx.compilerConfig.mode);
  }

  /*
   * Build the tile assignments.
   */
  if (ctx.compilerConfig.mode == CompilerMode::PRIMARY) {
    assignTilesPrimary(ctx, *compiled, indexedNormTilesWithColorSets, assignedPalsSolution);
  }
  else if (ctx.compilerConfig.mode == CompilerMode::SECONDARY) {
    assignTilesSecondary(ctx, *compiled, indexedNormTilesWithColorSets, primaryPaletteColorSets, assignedPalsSolution);
  }
  else if (ctx.compilerConfig.mode == CompilerMode::FREESTANDING) {
    throw std::runtime_error{"TODO : support FREESTANDING mode"};
  }
  else {
    internalerror_unknownCompilerMode(ctx.compilerConfig.mode);
  }

  return compiled;
}
} // namespace porytiles

// --------------------
// |    TEST CASES    |
// --------------------

TEST_CASE("insertRGBA should add new colors in order and return the correct index for a given color")
{
  porytiles::PtContext ctx{};

  porytiles::NormalizedPalette palette1{};
  palette1.size = 1;
  palette1.colors = {};

  // invalid alpha value, must be opaque or transparent
  CHECK_THROWS_WITH_AS(insertRGBA(ctx.compilerConfig.transparencyColor, palette1, porytiles::RGBA32{0, 0, 0, 12}),
                       "invalid alpha value: 12", const porytiles::PtException &);

  // Transparent should return 0
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1, porytiles::RGBA_MAGENTA) == 0);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_TRANSPARENT}) == 0);

  // insert colors
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_OPAQUE}) == 1);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{8, 0, 0, porytiles::ALPHA_OPAQUE}) == 2);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{16, 0, 0, porytiles::ALPHA_OPAQUE}) == 3);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{24, 0, 0, porytiles::ALPHA_OPAQUE}) == 4);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{32, 0, 0, porytiles::ALPHA_OPAQUE}) == 5);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{40, 0, 0, porytiles::ALPHA_OPAQUE}) == 6);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{48, 0, 0, porytiles::ALPHA_OPAQUE}) == 7);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{56, 0, 0, porytiles::ALPHA_OPAQUE}) == 8);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{64, 0, 0, porytiles::ALPHA_OPAQUE}) == 9);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{72, 0, 0, porytiles::ALPHA_OPAQUE}) == 10);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{80, 0, 0, porytiles::ALPHA_OPAQUE}) == 11);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{88, 0, 0, porytiles::ALPHA_OPAQUE}) == 12);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{96, 0, 0, porytiles::ALPHA_OPAQUE}) == 13);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{104, 0, 0, porytiles::ALPHA_OPAQUE}) == 14);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{112, 0, 0, porytiles::ALPHA_OPAQUE}) == 15);

  // repeat colors should return their indexes
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{72, 0, 0, porytiles::ALPHA_OPAQUE}) == 10);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{112, 0, 0, porytiles::ALPHA_OPAQUE}) == 15);

  // Transparent should still return 0
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1, porytiles::RGBA_MAGENTA) == 0);
  CHECK(insertRGBA(ctx.compilerConfig.transparencyColor, palette1,
                   porytiles::RGBA32{0, 0, 0, porytiles::ALPHA_TRANSPARENT}) == 0);

  // Should throw, palette full
  CHECK_THROWS_WITH_AS(insertRGBA(ctx.compilerConfig.transparencyColor, palette1, porytiles::RGBA_CYAN),
                       "too many unique colors in tile", const porytiles::PtException &);
}

TEST_CASE("candidate should return the NormalizedTile with requested flips")
{
  porytiles::PtContext ctx{};

  REQUIRE(std::filesystem::exists("res/tests/corners.png"));
  png::image<png::rgba_pixel> png1{"res/tests/corners.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  porytiles::RGBATile tile = tiles.tiles[0];

  SUBCASE("case: no flips")
  {
    porytiles::NormalizedTile candidate =
        porytiles::candidate(ctx.compilerConfig.transparencyColor, tile, false, false);
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
    CHECK(candidate.pixels.colorIndexes[0] == 1);
    CHECK(candidate.pixels.colorIndexes[7] == 2);
    CHECK(candidate.pixels.colorIndexes[9] == 3);
    CHECK(candidate.pixels.colorIndexes[14] == 4);
    CHECK(candidate.pixels.colorIndexes[18] == 2);
    CHECK(candidate.pixels.colorIndexes[21] == 5);
    CHECK(candidate.pixels.colorIndexes[42] == 3);
    CHECK(candidate.pixels.colorIndexes[45] == 1);
    CHECK(candidate.pixels.colorIndexes[49] == 6);
    CHECK(candidate.pixels.colorIndexes[54] == 7);
    CHECK(candidate.pixels.colorIndexes[56] == 8);
    CHECK(candidate.pixels.colorIndexes[63] == 5);
  }

  SUBCASE("case: hFlip")
  {
    porytiles::NormalizedTile candidate = porytiles::candidate(ctx.compilerConfig.transparencyColor, tile, true, false);
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
    CHECK(candidate.pixels.colorIndexes[0] == 1);
    CHECK(candidate.pixels.colorIndexes[7] == 2);
    CHECK(candidate.pixels.colorIndexes[9] == 3);
    CHECK(candidate.pixels.colorIndexes[14] == 4);
    CHECK(candidate.pixels.colorIndexes[18] == 5);
    CHECK(candidate.pixels.colorIndexes[21] == 1);
    CHECK(candidate.pixels.colorIndexes[42] == 2);
    CHECK(candidate.pixels.colorIndexes[45] == 4);
    CHECK(candidate.pixels.colorIndexes[49] == 6);
    CHECK(candidate.pixels.colorIndexes[54] == 7);
    CHECK(candidate.pixels.colorIndexes[56] == 5);
    CHECK(candidate.pixels.colorIndexes[63] == 8);
  }

  SUBCASE("case: vFlip")
  {
    porytiles::NormalizedTile candidate = porytiles::candidate(ctx.compilerConfig.transparencyColor, tile, false, true);
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
    CHECK(candidate.pixels.colorIndexes[0] == 1);
    CHECK(candidate.pixels.colorIndexes[7] == 2);
    CHECK(candidate.pixels.colorIndexes[9] == 3);
    CHECK(candidate.pixels.colorIndexes[14] == 4);
    CHECK(candidate.pixels.colorIndexes[18] == 5);
    CHECK(candidate.pixels.colorIndexes[21] == 6);
    CHECK(candidate.pixels.colorIndexes[42] == 7);
    CHECK(candidate.pixels.colorIndexes[45] == 2);
    CHECK(candidate.pixels.colorIndexes[49] == 5);
    CHECK(candidate.pixels.colorIndexes[54] == 8);
    CHECK(candidate.pixels.colorIndexes[56] == 6);
    CHECK(candidate.pixels.colorIndexes[63] == 7);
  }

  SUBCASE("case: hFlip and vFlip")
  {
    porytiles::NormalizedTile candidate = porytiles::candidate(ctx.compilerConfig.transparencyColor, tile, true, true);
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
    CHECK(candidate.pixels.colorIndexes[0] == 1);
    CHECK(candidate.pixels.colorIndexes[7] == 2);
    CHECK(candidate.pixels.colorIndexes[9] == 3);
    CHECK(candidate.pixels.colorIndexes[14] == 4);
    CHECK(candidate.pixels.colorIndexes[18] == 5);
    CHECK(candidate.pixels.colorIndexes[21] == 6);
    CHECK(candidate.pixels.colorIndexes[42] == 1);
    CHECK(candidate.pixels.colorIndexes[45] == 7);
    CHECK(candidate.pixels.colorIndexes[49] == 8);
    CHECK(candidate.pixels.colorIndexes[54] == 6);
    CHECK(candidate.pixels.colorIndexes[56] == 7);
    CHECK(candidate.pixels.colorIndexes[63] == 5);
  }
}

TEST_CASE("normalize should return the normal form of the given tile")
{
  porytiles::PtContext ctx{};

  REQUIRE(std::filesystem::exists("res/tests/corners.png"));
  png::image<png::rgba_pixel> png1{"res/tests/corners.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  porytiles::RGBATile tile = tiles.tiles[0];

  porytiles::NormalizedTile normalizedTile = porytiles::normalize(ctx.compilerConfig.transparencyColor, tile);
  CHECK(normalizedTile.palette.size == 9);
  CHECK_FALSE(normalizedTile.hFlip);
  CHECK_FALSE(normalizedTile.vFlip);
  CHECK(normalizedTile.pixels.colorIndexes[0] == 1);
  CHECK(normalizedTile.pixels.colorIndexes[7] == 2);
  CHECK(normalizedTile.pixels.colorIndexes[9] == 3);
  CHECK(normalizedTile.pixels.colorIndexes[14] == 4);
  CHECK(normalizedTile.pixels.colorIndexes[18] == 2);
  CHECK(normalizedTile.pixels.colorIndexes[21] == 5);
  CHECK(normalizedTile.pixels.colorIndexes[42] == 3);
  CHECK(normalizedTile.pixels.colorIndexes[45] == 1);
  CHECK(normalizedTile.pixels.colorIndexes[49] == 6);
  CHECK(normalizedTile.pixels.colorIndexes[54] == 7);
  CHECK(normalizedTile.pixels.colorIndexes[56] == 8);
  CHECK(normalizedTile.pixels.colorIndexes[63] == 5);
}

TEST_CASE("normalizeDecompTiles should correctly normalize all tiles in the decomp tileset")
{
  porytiles::PtContext ctx{};

  REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
  png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);

  std::vector<IndexedNormTile> indexedNormTiles = normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);

  CHECK(indexedNormTiles.size() == 4);

  // First tile normal form is vFlipped, palette should have 2 colors
  CHECK(indexedNormTiles[0].second.pixels.colorIndexes[0] == 0);
  CHECK(indexedNormTiles[0].second.pixels.colorIndexes[7] == 1);
  for (int i = 56; i <= 63; i++) {
    CHECK(indexedNormTiles[0].second.pixels.colorIndexes[i] == 1);
  }
  CHECK(indexedNormTiles[0].second.palette.size == 2);
  CHECK(indexedNormTiles[0].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(indexedNormTiles[0].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK_FALSE(indexedNormTiles[0].second.hFlip);
  CHECK(indexedNormTiles[0].second.vFlip);
  CHECK(indexedNormTiles[0].first == 0);

  // Second tile already in normal form, palette should have 3 colors
  CHECK(indexedNormTiles[1].second.pixels.colorIndexes[0] == 0);
  CHECK(indexedNormTiles[1].second.pixels.colorIndexes[54] == 1);
  CHECK(indexedNormTiles[1].second.pixels.colorIndexes[55] == 1);
  CHECK(indexedNormTiles[1].second.pixels.colorIndexes[62] == 1);
  CHECK(indexedNormTiles[1].second.pixels.colorIndexes[63] == 2);
  CHECK(indexedNormTiles[1].second.palette.size == 3);
  CHECK(indexedNormTiles[1].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(indexedNormTiles[1].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK(indexedNormTiles[1].second.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
  CHECK_FALSE(indexedNormTiles[1].second.hFlip);
  CHECK_FALSE(indexedNormTiles[1].second.vFlip);
  CHECK(indexedNormTiles[1].first == 1);

  // Third tile normal form is hFlipped, palette should have 3 colors
  CHECK(indexedNormTiles[2].second.pixels.colorIndexes[0] == 0);
  CHECK(indexedNormTiles[2].second.pixels.colorIndexes[7] == 1);
  CHECK(indexedNormTiles[2].second.pixels.colorIndexes[56] == 1);
  CHECK(indexedNormTiles[2].second.pixels.colorIndexes[63] == 2);
  CHECK(indexedNormTiles[2].second.palette.size == 3);
  CHECK(indexedNormTiles[2].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(indexedNormTiles[2].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
  CHECK(indexedNormTiles[2].second.palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK_FALSE(indexedNormTiles[2].second.vFlip);
  CHECK(indexedNormTiles[2].second.hFlip);
  CHECK(indexedNormTiles[2].first == 2);

  // Fourth tile normal form is hFlipped and vFlipped, palette should have 2 colors
  CHECK(indexedNormTiles[3].second.pixels.colorIndexes[0] == 0);
  CHECK(indexedNormTiles[3].second.pixels.colorIndexes[7] == 1);
  for (int i = 56; i <= 63; i++) {
    CHECK(indexedNormTiles[3].second.pixels.colorIndexes[i] == 1);
  }
  CHECK(indexedNormTiles[3].second.palette.size == 2);
  CHECK(indexedNormTiles[3].second.palette.colors[0] == porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(indexedNormTiles[3].second.palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(indexedNormTiles[3].second.hFlip);
  CHECK(indexedNormTiles[3].second.vFlip);
  CHECK(indexedNormTiles[3].first == 3);
}

TEST_CASE("buildColorIndexMaps should build a map of all unique colors in the decomp tileset")
{
  porytiles::PtContext ctx{};

  REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
  png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  std::vector<IndexedNormTile> normalizedTiles =
      porytiles::normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);

  auto [colorToIndex, indexToColor] = porytiles::buildColorIndexMaps(ctx, normalizedTiles, {});

  CHECK(colorToIndex.size() == 4);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 0);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 1);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 2);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_CYAN)] == 3);
}

TEST_CASE("toColorSet should return the correct bitset based on the supplied palette")
{
  std::unordered_map<porytiles::BGR15, std::size_t> colorIndexMap = {
      {porytiles::rgbaToBgr(porytiles::RGBA_BLUE), 0},   {porytiles::rgbaToBgr(porytiles::RGBA_RED), 1},
      {porytiles::rgbaToBgr(porytiles::RGBA_GREEN), 2},  {porytiles::rgbaToBgr(porytiles::RGBA_CYAN), 3},
      {porytiles::rgbaToBgr(porytiles::RGBA_YELLOW), 4},
  };

  SUBCASE("palette 1")
  {
    porytiles::NormalizedPalette palette{};
    palette.size = 2;
    palette.colors[0] = porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA);
    palette.colors[1] = porytiles::rgbaToBgr(porytiles::RGBA_RED);

    ColorSet colorSet = porytiles::toColorSet(colorIndexMap, palette);
    CHECK(colorSet.count() == 1);
    CHECK(colorSet.test(1));
  }

  SUBCASE("palette 2")
  {
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

TEST_CASE("matchNormalizedWithColorSets should return the expected data structures")
{
  porytiles::PtContext ctx{};

  REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
  png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  std::vector<IndexedNormTile> indexedNormTiles =
      porytiles::normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);
  auto [colorToIndex, indexToColor] = porytiles::buildColorIndexMaps(ctx, indexedNormTiles, {});

  CHECK(colorToIndex.size() == 4);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 0);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 1);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 2);
  CHECK(colorToIndex[porytiles::rgbaToBgr(porytiles::RGBA_CYAN)] == 3);

  auto [indexedNormTilesWithColorSets, colorSets] =
      porytiles::matchNormalizedWithColorSets(colorToIndex, indexedNormTiles);

  CHECK(indexedNormTilesWithColorSets.size() == 4);
  // colorSets size is 3 because first and fourth tiles have the same palette
  CHECK(colorSets.size() == 3);

  // First tile has 1 non-transparent color, color should be BLUE
  CHECK(std::get<0>(indexedNormTilesWithColorSets[0]) == 0);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.colorIndexes[0] == 0);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.colorIndexes[7] == 1);
  for (int i = 56; i <= 63; i++) {
    CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).pixels.colorIndexes[i] == 1);
  }
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.size == 2);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.colors[0] ==
        porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[0]).hFlip);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[0]).vFlip);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[0]).count() == 1);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[0]).test(0));
  CHECK(std::find(colorSets.begin(), colorSets.end(), std::get<2>(indexedNormTilesWithColorSets[0])) !=
        colorSets.end());

  // Second tile has two non-transparent colors, RED and GREEN
  CHECK(std::get<0>(indexedNormTilesWithColorSets[1]) == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.colorIndexes[0] == 0);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.colorIndexes[54] == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.colorIndexes[55] == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.colorIndexes[62] == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).pixels.colorIndexes[63] == 2);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.size == 3);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[0] ==
        porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[1]).palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
  CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[1]).hFlip);
  CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[1]).vFlip);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).count() == 2);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).test(1));
  CHECK(std::get<2>(indexedNormTilesWithColorSets[1]).test(2));
  CHECK(std::find(colorSets.begin(), colorSets.end(), std::get<2>(indexedNormTilesWithColorSets[1])) !=
        colorSets.end());

  // Third tile has two non-transparent colors, CYAN and GREEN
  CHECK(std::get<0>(indexedNormTilesWithColorSets[2]) == 2);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.colorIndexes[0] == 0);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.colorIndexes[7] == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.colorIndexes[56] == 1);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).pixels.colorIndexes[63] == 2);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.size == 3);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[0] ==
        porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).palette.colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK_FALSE(std::get<1>(indexedNormTilesWithColorSets[2]).vFlip);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[2]).hFlip);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).count() == 2);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).test(1));
  CHECK(std::get<2>(indexedNormTilesWithColorSets[2]).test(3));
  CHECK(std::find(colorSets.begin(), colorSets.end(), std::get<2>(indexedNormTilesWithColorSets[2])) !=
        colorSets.end());

  // Fourth tile has 1 non-transparent color, color should be BLUE
  CHECK(std::get<0>(indexedNormTilesWithColorSets[3]) == 3);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.colorIndexes[0] == 0);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.colorIndexes[7] == 1);
  for (int i = 56; i <= 63; i++) {
    CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).pixels.colorIndexes[i] == 1);
  }
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.size == 2);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.colors[0] ==
        porytiles::rgbaToBgr(porytiles::RGBA_MAGENTA));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).palette.colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).hFlip);
  CHECK(std::get<1>(indexedNormTilesWithColorSets[3]).vFlip);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[3]).count() == 1);
  CHECK(std::get<2>(indexedNormTilesWithColorSets[3]).test(0));
  CHECK(std::find(colorSets.begin(), colorSets.end(), std::get<2>(indexedNormTilesWithColorSets[3])) !=
        colorSets.end());
}

TEST_CASE("assign should correctly assign all normalized palettes or fail if impossible")
{
  porytiles::PtContext ctx{};
  ctx.compilerConfig.mode = porytiles::CompilerMode::PRIMARY;

  SUBCASE("It should successfully allocate a simple 2x2 tileset png")
  {
    constexpr int SOLUTION_SIZE = 2;
    ctx.fieldmapConfig.numPalettesInPrimary = SOLUTION_SIZE;
    ctx.compilerConfig.maxRecurseCount = 20;

    REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
    png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
    porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
    std::vector<IndexedNormTile> indexedNormTiles =
        porytiles::normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);
    auto [colorToIndex, indexToColor] = porytiles::buildColorIndexMaps(ctx, indexedNormTiles, {});
    auto [indexedNormTilesWithColorSets, colorSets] =
        porytiles::matchNormalizedWithColorSets(colorToIndex, indexedNormTiles);

    // Set up the state struct
    std::vector<ColorSet> solution;
    solution.reserve(SOLUTION_SIZE);
    std::vector<ColorSet> hardwarePalettes;
    hardwarePalettes.resize(SOLUTION_SIZE);
    std::vector<ColorSet> unassigned;
    std::copy(std::begin(colorSets), std::end(colorSets), std::back_inserter(unassigned));
    std::stable_sort(std::begin(unassigned), std::end(unassigned),
                     [](const auto &cs1, const auto &cs2) { return cs1.count() < cs2.count(); });
    porytiles::AssignState state = {hardwarePalettes, unassigned};

    porytiles::gRecurseCount = 0;
    CHECK(porytiles::assign(ctx.compilerConfig.maxRecurseCount, state, solution, {}));
    CHECK(solution.size() == SOLUTION_SIZE);
    CHECK(solution.at(0).count() == 1);
    CHECK(solution.at(1).count() == 3);
    CHECK(solution.at(0).test(0));
    CHECK(solution.at(1).test(1));
    CHECK(solution.at(1).test(2));
    CHECK(solution.at(1).test(3));
  }

  SUBCASE("It should successfully allocate a large, complex PNG")
  {
    constexpr int SOLUTION_SIZE = 5;
    ctx.fieldmapConfig.numPalettesInPrimary = SOLUTION_SIZE;
    ctx.compilerConfig.maxRecurseCount = 200;

    REQUIRE(std::filesystem::exists("res/tests/compile_raw_set_1/set.png"));
    png::image<png::rgba_pixel> png1{"res/tests/compile_raw_set_1/set.png"};
    porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
    std::vector<IndexedNormTile> indexedNormTiles =
        porytiles::normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);
    auto [colorToIndex, indexToColor] = porytiles::buildColorIndexMaps(ctx, indexedNormTiles, {});
    auto [indexedNormTilesWithColorSets, colorSets] =
        porytiles::matchNormalizedWithColorSets(colorToIndex, indexedNormTiles);

    // Set up the state struct
    std::vector<ColorSet> solution;
    solution.reserve(SOLUTION_SIZE);
    std::vector<ColorSet> hardwarePalettes;
    hardwarePalettes.resize(SOLUTION_SIZE);
    std::vector<ColorSet> unassigned;
    std::copy(std::begin(colorSets), std::end(colorSets), std::back_inserter(unassigned));
    std::stable_sort(std::begin(unassigned), std::end(unassigned),
                     [](const auto &cs1, const auto &cs2) { return cs1.count() < cs2.count(); });
    porytiles::AssignState state = {hardwarePalettes, unassigned};

    porytiles::gRecurseCount = 0;
    CHECK(porytiles::assign(ctx.compilerConfig.maxRecurseCount, state, solution, {}));
    CHECK(solution.size() == SOLUTION_SIZE);
    CHECK(solution.at(0).count() == 11);
    CHECK(solution.at(1).count() == 12);
    CHECK(solution.at(2).count() == 14);
    CHECK(solution.at(3).count() == 14);
    CHECK(solution.at(4).count() == 15);
  }
}

TEST_CASE("makeTile should create the expected GBATile from the given NormalizedTile and GBAPalette")
{
  porytiles::PtContext ctx{};
  ctx.compilerConfig.transparencyColor = porytiles::RGBA_MAGENTA;
  ctx.fieldmapConfig.numPalettesInPrimary = 2;
  ctx.fieldmapConfig.numTilesInPrimary = 4;
  ctx.compilerConfig.maxRecurseCount = 5;
  ctx.compilerConfig.mode = porytiles::CompilerMode::PRIMARY;

  REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
  png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  std::vector<IndexedNormTile> indexedNormTiles = normalizeDecompTiles(ctx.compilerConfig.transparencyColor, tiles);
  auto compiledTiles = porytiles::compile(ctx, tiles);

  porytiles::GBATile tile0 = porytiles::makeTile(indexedNormTiles[0].second, compiledTiles->palettes[0]);
  CHECK_FALSE(indexedNormTiles[0].second.hFlip);
  CHECK(indexedNormTiles[0].second.vFlip);
  CHECK(tile0.colorIndexes[0] == 0);
  CHECK(tile0.colorIndexes[7] == 1);
  for (size_t i = 56; i < 64; i++) {
    CHECK(tile0.colorIndexes[i] == 1);
  }

  porytiles::GBATile tile1 = porytiles::makeTile(indexedNormTiles[1].second, compiledTiles->palettes[1]);
  CHECK_FALSE(indexedNormTiles[1].second.hFlip);
  CHECK_FALSE(indexedNormTiles[1].second.vFlip);
  CHECK(tile1.colorIndexes[0] == 0);
  CHECK(tile1.colorIndexes[54] == 1);
  CHECK(tile1.colorIndexes[55] == 1);
  CHECK(tile1.colorIndexes[62] == 1);
  CHECK(tile1.colorIndexes[63] == 2);

  porytiles::GBATile tile2 = porytiles::makeTile(indexedNormTiles[2].second, compiledTiles->palettes[1]);
  CHECK(indexedNormTiles[2].second.hFlip);
  CHECK_FALSE(indexedNormTiles[2].second.vFlip);
  CHECK(tile2.colorIndexes[0] == 0);
  CHECK(tile2.colorIndexes[7] == 3);
  CHECK(tile2.colorIndexes[56] == 3);
  CHECK(tile2.colorIndexes[63] == 1);

  porytiles::GBATile tile3 = porytiles::makeTile(indexedNormTiles[3].second, compiledTiles->palettes[0]);
  CHECK(indexedNormTiles[3].second.hFlip);
  CHECK(indexedNormTiles[3].second.vFlip);
  CHECK(tile3.colorIndexes[0] == 0);
  CHECK(tile3.colorIndexes[7] == 1);
  for (size_t i = 56; i < 64; i++) {
    CHECK(tile3.colorIndexes[i] == 1);
  }
}

TEST_CASE("compile simple example should perform as expected")
{
  porytiles::PtContext ctx{};
  ctx.fieldmapConfig.numPalettesInPrimary = 2;
  ctx.fieldmapConfig.numTilesInPrimary = 4;
  ctx.compilerConfig.maxRecurseCount = 5;
  ctx.compilerConfig.mode = porytiles::CompilerMode::PRIMARY;

  REQUIRE(std::filesystem::exists("res/tests/2x2_pattern_2.png"));
  png::image<png::rgba_pixel> png1{"res/tests/2x2_pattern_2.png"};
  porytiles::DecompiledTileset tiles = porytiles::importRawTilesFromPng(png1);
  auto compiledTiles = porytiles::compile(ctx, tiles);

  // Check that compiled palettes are as expected
  CHECK(compiledTiles->palettes.at(0).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledTiles->palettes.at(0).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(compiledTiles->palettes.at(1).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledTiles->palettes.at(1).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK(compiledTiles->palettes.at(1).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
  CHECK(compiledTiles->palettes.at(1).colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));

  /*
   * Check that compiled GBATiles have expected index values, there are only 3 in final tileset (ignoring the
   * transparent tile at the start) since two of the original tiles are flips of each other.
   */
  porytiles::GBATile &tile0 = compiledTiles->tiles[0];
  for (size_t i = 0; i < 64; i++) {
    CHECK(tile0.colorIndexes[i] == 0);
  }

  porytiles::GBATile &tile1 = compiledTiles->tiles[1];
  CHECK(tile1.colorIndexes[0] == 0);
  CHECK(tile1.colorIndexes[7] == 1);
  for (size_t i = 56; i < 64; i++) {
    CHECK(tile1.colorIndexes[i] == 1);
  }

  porytiles::GBATile tile2 = compiledTiles->tiles[2];
  CHECK(tile2.colorIndexes[0] == 0);
  CHECK(tile2.colorIndexes[54] == 1);
  CHECK(tile2.colorIndexes[55] == 1);
  CHECK(tile2.colorIndexes[62] == 1);
  CHECK(tile2.colorIndexes[63] == 2);

  porytiles::GBATile tile3 = compiledTiles->tiles[3];
  CHECK(tile3.colorIndexes[0] == 0);
  CHECK(tile3.colorIndexes[7] == 3);
  CHECK(tile3.colorIndexes[56] == 3);
  CHECK(tile3.colorIndexes[63] == 1);

  /*
   * Check that all the assignments are correct.
   */
  CHECK(compiledTiles->assignments[0].tileIndex == 1);
  CHECK(compiledTiles->assignments[0].paletteIndex == 0);
  CHECK_FALSE(compiledTiles->assignments[0].hFlip);
  CHECK(compiledTiles->assignments[0].vFlip);

  CHECK(compiledTiles->assignments[1].tileIndex == 2);
  CHECK(compiledTiles->assignments[1].paletteIndex == 1);
  CHECK_FALSE(compiledTiles->assignments[1].hFlip);
  CHECK_FALSE(compiledTiles->assignments[1].vFlip);

  CHECK(compiledTiles->assignments[2].tileIndex == 3);
  CHECK(compiledTiles->assignments[2].paletteIndex == 1);
  CHECK(compiledTiles->assignments[2].hFlip);
  CHECK_FALSE(compiledTiles->assignments[2].vFlip);

  CHECK(compiledTiles->assignments[3].tileIndex == 1);
  CHECK(compiledTiles->assignments[3].paletteIndex == 0);
  CHECK(compiledTiles->assignments[3].hFlip);
  CHECK(compiledTiles->assignments[3].vFlip);
}

TEST_CASE("compile function should fill out CompiledTileset struct with expected values")
{
  porytiles::PtContext ctx{};
  ctx.fieldmapConfig.numPalettesInPrimary = 3;
  ctx.fieldmapConfig.numPalettesTotal = 6;
  ctx.compilerConfig.mode = porytiles::CompilerMode::PRIMARY;

  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/bottom_primary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/middle_primary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/top_primary.png"));
  png::image<png::rgba_pixel> bottomPrimary{"res/tests/simple_metatiles_3/bottom_primary.png"};
  png::image<png::rgba_pixel> middlePrimary{"res/tests/simple_metatiles_3/middle_primary.png"};
  png::image<png::rgba_pixel> topPrimary{"res/tests/simple_metatiles_3/top_primary.png"};
  porytiles::DecompiledTileset decompiledPrimary =
      porytiles::importLayeredTilesFromPngs(ctx, bottomPrimary, middlePrimary, topPrimary);

  auto compiledPrimary = porytiles::compile(ctx, decompiledPrimary);

  // Check that tiles are as expected
  CHECK(compiledPrimary->tiles.size() == 5);
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/primary_expected_tiles.png"));
  png::image<png::index_pixel> expectedPng{"res/tests/simple_metatiles_3/primary_expected_tiles.png"};
  for (std::size_t tileIndex = 0; tileIndex < compiledPrimary->tiles.size(); tileIndex++) {
    for (std::size_t row = 0; row < porytiles::TILE_SIDE_LENGTH; row++) {
      for (std::size_t col = 0; col < porytiles::TILE_SIDE_LENGTH; col++) {
        CHECK(compiledPrimary->tiles[tileIndex].colorIndexes[col + (row * porytiles::TILE_SIDE_LENGTH)] ==
              expectedPng[row][col + (tileIndex * porytiles::TILE_SIDE_LENGTH)]);
      }
    }
  }

  // Check that paletteIndexesOfTile are correct
  CHECK(compiledPrimary->paletteIndexesOfTile.size() == 5);
  CHECK(compiledPrimary->paletteIndexesOfTile[0] == 0);
  CHECK(compiledPrimary->paletteIndexesOfTile[1] == 2);
  CHECK(compiledPrimary->paletteIndexesOfTile[2] == 1);
  CHECK(compiledPrimary->paletteIndexesOfTile[3] == 1);
  CHECK(compiledPrimary->paletteIndexesOfTile[4] == 0);

  // Check that compiled palettes are as expected
  CHECK(compiledPrimary->palettes.size() == ctx.fieldmapConfig.numPalettesInPrimary);
  CHECK(compiledPrimary->palettes.at(0).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledPrimary->palettes.at(0).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
  CHECK(compiledPrimary->palettes.at(1).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledPrimary->palettes.at(1).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK(compiledPrimary->palettes.at(1).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(compiledPrimary->palettes.at(2).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledPrimary->palettes.at(2).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
  CHECK(compiledPrimary->palettes.at(2).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));

  // Check that all assignments are correct
  CHECK(compiledPrimary->assignments.size() == porytiles::METATILES_IN_ROW * ctx.fieldmapConfig.numTilesPerMetatile);

  CHECK(compiledPrimary->assignments[0].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[0].vFlip);
  CHECK(compiledPrimary->assignments[0].tileIndex == 1);
  CHECK(compiledPrimary->assignments[0].paletteIndex == 2);

  CHECK_FALSE(compiledPrimary->assignments[1].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[1].vFlip);
  CHECK(compiledPrimary->assignments[1].tileIndex == 0);
  CHECK(compiledPrimary->assignments[1].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[2].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[2].vFlip);
  CHECK(compiledPrimary->assignments[2].tileIndex == 0);
  CHECK(compiledPrimary->assignments[2].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[3].hFlip);
  CHECK(compiledPrimary->assignments[3].vFlip);
  CHECK(compiledPrimary->assignments[3].tileIndex == 2);
  CHECK(compiledPrimary->assignments[3].paletteIndex == 1);

  CHECK_FALSE(compiledPrimary->assignments[4].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[4].vFlip);
  CHECK(compiledPrimary->assignments[4].tileIndex == 0);
  CHECK(compiledPrimary->assignments[4].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[5].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[5].vFlip);
  CHECK(compiledPrimary->assignments[5].tileIndex == 0);
  CHECK(compiledPrimary->assignments[5].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[6].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[6].vFlip);
  CHECK(compiledPrimary->assignments[6].tileIndex == 3);
  CHECK(compiledPrimary->assignments[6].paletteIndex == 1);

  CHECK_FALSE(compiledPrimary->assignments[7].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[7].vFlip);
  CHECK(compiledPrimary->assignments[7].tileIndex == 0);
  CHECK(compiledPrimary->assignments[7].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[8].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[8].vFlip);
  CHECK(compiledPrimary->assignments[8].tileIndex == 0);
  CHECK(compiledPrimary->assignments[8].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[9].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[9].vFlip);
  CHECK(compiledPrimary->assignments[9].tileIndex == 4);
  CHECK(compiledPrimary->assignments[9].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[10].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[10].vFlip);
  CHECK(compiledPrimary->assignments[10].tileIndex == 0);
  CHECK(compiledPrimary->assignments[10].paletteIndex == 0);

  CHECK_FALSE(compiledPrimary->assignments[11].hFlip);
  CHECK_FALSE(compiledPrimary->assignments[11].vFlip);
  CHECK(compiledPrimary->assignments[11].tileIndex == 0);
  CHECK(compiledPrimary->assignments[11].paletteIndex == 0);

  for (std::size_t index = ctx.fieldmapConfig.numTilesPerMetatile;
       index < porytiles::METATILES_IN_ROW * ctx.fieldmapConfig.numTilesPerMetatile; index++) {
    CHECK_FALSE(compiledPrimary->assignments[index].hFlip);
    CHECK_FALSE(compiledPrimary->assignments[index].vFlip);
    CHECK(compiledPrimary->assignments[index].tileIndex == 0);
    CHECK(compiledPrimary->assignments[index].paletteIndex == 0);
  }

  // Check that colorIndexMap is correct
  CHECK(compiledPrimary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 0);
  CHECK(compiledPrimary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_YELLOW)] == 1);
  CHECK(compiledPrimary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 2);
  CHECK(compiledPrimary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 3);
  CHECK(compiledPrimary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_WHITE)] == 4);

  // Check that tileIndexes is correct
  CHECK(compiledPrimary->tileIndexes.size() == compiledPrimary->tiles.size());
  CHECK(compiledPrimary->tileIndexes[compiledPrimary->tiles[0]] == 0);
  CHECK(compiledPrimary->tileIndexes[compiledPrimary->tiles[1]] == 1);
  CHECK(compiledPrimary->tileIndexes[compiledPrimary->tiles[2]] == 2);
  CHECK(compiledPrimary->tileIndexes[compiledPrimary->tiles[3]] == 3);
  CHECK(compiledPrimary->tileIndexes[compiledPrimary->tiles[4]] == 4);
}

TEST_CASE("compileSecondary function should fill out CompiledTileset struct with expected values")
{
  porytiles::PtContext ctx{};
  ctx.fieldmapConfig.numPalettesInPrimary = 3;
  ctx.fieldmapConfig.numPalettesTotal = 6;
  ctx.compilerConfig.mode = porytiles::CompilerMode::PRIMARY;

  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/bottom_primary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/middle_primary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/top_primary.png"));
  png::image<png::rgba_pixel> bottomPrimary{"res/tests/simple_metatiles_3/bottom_primary.png"};
  png::image<png::rgba_pixel> middlePrimary{"res/tests/simple_metatiles_3/middle_primary.png"};
  png::image<png::rgba_pixel> topPrimary{"res/tests/simple_metatiles_3/top_primary.png"};
  porytiles::DecompiledTileset decompiledPrimary =
      porytiles::importLayeredTilesFromPngs(ctx, bottomPrimary, middlePrimary, topPrimary);

  auto compiledPrimary = porytiles::compile(ctx, decompiledPrimary);

  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/bottom_secondary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/middle_secondary.png"));
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/top_secondary.png"));
  png::image<png::rgba_pixel> bottomSecondary{"res/tests/simple_metatiles_3/bottom_secondary.png"};
  png::image<png::rgba_pixel> middleSecondary{"res/tests/simple_metatiles_3/middle_secondary.png"};
  png::image<png::rgba_pixel> topSecondary{"res/tests/simple_metatiles_3/top_secondary.png"};
  porytiles::DecompiledTileset decompiledSecondary =
      porytiles::importLayeredTilesFromPngs(ctx, bottomSecondary, middleSecondary, topSecondary);
  ctx.compilerConfig.mode = porytiles::CompilerMode::SECONDARY;
  ctx.compilerContext.pairedPrimaryTiles = std::move(compiledPrimary);
  auto compiledSecondary = porytiles::compile(ctx, decompiledSecondary);

  // Check that tiles are as expected
  REQUIRE(std::filesystem::exists("res/tests/simple_metatiles_3/secondary_expected_tiles.png"));
  png::image<png::index_pixel> expectedPng{"res/tests/simple_metatiles_3/secondary_expected_tiles.png"};
  for (std::size_t tileIndex = 0; tileIndex < compiledSecondary->tiles.size(); tileIndex++) {
    for (std::size_t row = 0; row < porytiles::TILE_SIDE_LENGTH; row++) {
      for (std::size_t col = 0; col < porytiles::TILE_SIDE_LENGTH; col++) {
        CHECK(compiledSecondary->tiles[tileIndex].colorIndexes[col + (row * porytiles::TILE_SIDE_LENGTH)] ==
              expectedPng[row][col + (tileIndex * porytiles::TILE_SIDE_LENGTH)]);
      }
    }
  }

  // Check that paletteIndexesOfTile are correct
  CHECK(compiledSecondary->paletteIndexesOfTile[0] == 2);
  CHECK(compiledSecondary->paletteIndexesOfTile[1] == 3);
  CHECK(compiledSecondary->paletteIndexesOfTile[2] == 3);
  CHECK(compiledSecondary->paletteIndexesOfTile[3] == 3);
  CHECK(compiledSecondary->paletteIndexesOfTile[4] == 3);
  CHECK(compiledSecondary->paletteIndexesOfTile[5] == 5);

  // Check that compiled palettes are as expected
  CHECK(compiledSecondary->palettes.at(0).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(0).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_WHITE));
  CHECK(compiledSecondary->palettes.at(1).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(1).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREEN));
  CHECK(compiledSecondary->palettes.at(1).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(compiledSecondary->palettes.at(2).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(2).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_RED));
  CHECK(compiledSecondary->palettes.at(2).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_YELLOW));
  CHECK(compiledSecondary->palettes.at(3).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(3).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_BLUE));
  CHECK(compiledSecondary->palettes.at(3).colors[2] == porytiles::rgbaToBgr(porytiles::RGBA_CYAN));
  CHECK(compiledSecondary->palettes.at(3).colors[3] == porytiles::rgbaToBgr(porytiles::RGBA_PURPLE));
  CHECK(compiledSecondary->palettes.at(3).colors[4] == porytiles::rgbaToBgr(porytiles::RGBA_LIME));
  CHECK(compiledSecondary->palettes.at(4).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(5).colors[0] == porytiles::rgbaToBgr(ctx.compilerConfig.transparencyColor));
  CHECK(compiledSecondary->palettes.at(5).colors[1] == porytiles::rgbaToBgr(porytiles::RGBA_GREY));

  // Check that all assignments are correct
  CHECK(compiledSecondary->assignments.size() == porytiles::METATILES_IN_ROW * ctx.fieldmapConfig.numTilesPerMetatile);

  CHECK_FALSE(compiledSecondary->assignments[0].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[0].vFlip);
  CHECK(compiledSecondary->assignments[0].tileIndex == 0);
  CHECK(compiledSecondary->assignments[0].paletteIndex == 0);

  CHECK_FALSE(compiledSecondary->assignments[1].hFlip);
  CHECK(compiledSecondary->assignments[1].vFlip);
  CHECK(compiledSecondary->assignments[1].tileIndex == 0 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[1].paletteIndex == 2);

  CHECK_FALSE(compiledSecondary->assignments[2].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[2].vFlip);
  CHECK(compiledSecondary->assignments[2].tileIndex == 1 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[2].paletteIndex == 3);

  CHECK_FALSE(compiledSecondary->assignments[3].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[3].vFlip);
  CHECK(compiledSecondary->assignments[3].tileIndex == 0);
  CHECK(compiledSecondary->assignments[3].paletteIndex == 0);

  CHECK_FALSE(compiledSecondary->assignments[4].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[4].vFlip);
  CHECK(compiledSecondary->assignments[4].tileIndex == 0);
  CHECK(compiledSecondary->assignments[4].paletteIndex == 0);

  CHECK_FALSE(compiledSecondary->assignments[5].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[5].vFlip);
  CHECK(compiledSecondary->assignments[5].tileIndex == 2 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[5].paletteIndex == 3);

  CHECK_FALSE(compiledSecondary->assignments[6].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[6].vFlip);
  CHECK(compiledSecondary->assignments[6].tileIndex == 3 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[6].paletteIndex == 3);

  CHECK_FALSE(compiledSecondary->assignments[7].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[7].vFlip);
  CHECK(compiledSecondary->assignments[7].tileIndex == 0);
  CHECK(compiledSecondary->assignments[7].paletteIndex == 0);

  CHECK_FALSE(compiledSecondary->assignments[8].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[8].vFlip);
  CHECK(compiledSecondary->assignments[8].tileIndex == 4 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[8].paletteIndex == 3);

  CHECK_FALSE(compiledSecondary->assignments[9].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[9].vFlip);
  CHECK(compiledSecondary->assignments[9].tileIndex == 0);
  CHECK(compiledSecondary->assignments[9].paletteIndex == 0);

  CHECK_FALSE(compiledSecondary->assignments[10].hFlip);
  CHECK_FALSE(compiledSecondary->assignments[10].vFlip);
  CHECK(compiledSecondary->assignments[10].tileIndex == 0);
  CHECK(compiledSecondary->assignments[10].paletteIndex == 0);

  CHECK(compiledSecondary->assignments[11].hFlip);
  CHECK(compiledSecondary->assignments[11].vFlip);
  CHECK(compiledSecondary->assignments[11].tileIndex == 5 + ctx.fieldmapConfig.numTilesInPrimary);
  CHECK(compiledSecondary->assignments[11].paletteIndex == 5);

  for (std::size_t index = ctx.fieldmapConfig.numTilesPerMetatile;
       index < porytiles::METATILES_IN_ROW * ctx.fieldmapConfig.numTilesPerMetatile; index++) {
    CHECK_FALSE(compiledSecondary->assignments[index].hFlip);
    CHECK_FALSE(compiledSecondary->assignments[index].vFlip);
    CHECK(compiledSecondary->assignments[index].tileIndex == 0);
    CHECK(compiledSecondary->assignments[index].paletteIndex == 0);
  }

  // Check that colorIndexMap is correct
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_RED)] == 0);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_YELLOW)] == 1);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_GREEN)] == 2);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_BLUE)] == 3);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_WHITE)] == 4);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_CYAN)] == 5);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_PURPLE)] == 6);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_LIME)] == 7);
  CHECK(compiledSecondary->colorIndexMap[porytiles::rgbaToBgr(porytiles::RGBA_GREY)] == 8);

  // Check that tileIndexes is correct
  CHECK(compiledSecondary->tileIndexes.size() == compiledSecondary->tiles.size());
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[0]] == 0);
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[1]] == 1);
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[2]] == 2);
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[3]] == 3);
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[4]] == 4);
  CHECK(compiledSecondary->tileIndexes[compiledSecondary->tiles[5]] == 5);
}
