#ifndef PORYTILES_EMITTER_H
#define PORYTILES_EMITTER_H

#include <iostream>
#include <png.hpp>

#include "ptcontext.h"
#include "types.h"

/**
 * TODO : fill in doc comment
 */

namespace porytiles {

extern const std::size_t TILES_PNG_WIDTH_IN_TILES;

/**
 * TODO : fill in doc comment
 */
void emitPalette(PtContext &ctx, std::ostream &out, const GBAPalette &palette);

/**
 * TODO : fill in doc comment
 */
void emitZeroedPalette(PtContext &ctx, std::ostream &out);

/**
 * TODO : fill in doc comment
 */
void emitTilesPng(PtContext &ctx, png::image<png::index_pixel> &out, const CompiledTileset &tileset);

/**
 * TODO : fill in doc comment
 */
void emitMetatilesBin(PtContext &ctx, std::ostream &out, const CompiledTileset &tileset);

/**
 * TODO : fill in doc comment
 */
void emitAnim(PtContext &ctx, std::vector<png::image<png::index_pixel>> &outFrames, const CompiledAnimation &animation,
              const std::vector<GBAPalette> &palettes);

/**
 * TODO : fill in doc comment
 */
void emitAttributes(PtContext &ctx, std::ostream &out, std::unordered_map<std::uint8_t, std::string> behaviorReverseMap,
                    const CompiledTileset &tileset);

} // namespace porytiles

#endif // PORYTILES_EMITTER_H
