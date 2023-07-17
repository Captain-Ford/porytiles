#include "config.h"

namespace porytiles {

Config defaultConfig() {
    Config config;

    config.numTilesInPrimary = 512;
    config.numTilesTotal = 1024;
    config.numMetatilesInPrimary = 512;
    config.numMetatilesTotal = 1024;
    config.numPalettesInPrimary = 6;
    config.numPalettesTotal = 13;
    config.numTilesPerMetatile = 8;
    config.secondary = false;

    config.transparencyColor = RGBA_MAGENTA;

    config.tilesPngPaletteMode = GREYSCALE;

    config.subcommand = COMPILE_RAW;

    return config;
}

}