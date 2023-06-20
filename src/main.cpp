#include "png_checks.h"
#include "cli_parser.h"
#include "tsexception.h"
#include "tsoutput.h"
#include "rgb_tiled_png.h"
#include "tileset.h"

#include <iostream>
#include <png.hpp>

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

namespace porytiles {
std::string errorPrefix() {
    std::string program(PROGRAM_NAME);
    return program + ": error: ";
}

std::string fatalPrefix() {
    std::string program(PROGRAM_NAME);
    return program + ": fatal: ";
}
} // namespace porytiles

int main(int argc, char** argv) try {
    // Parse CLI options and args, fill out global opt vars with expected values
    porytiles::parseOptions(argc, argv);

    // Verify that master PNG path points at a valid PNG file
    porytiles::validateMasterPngIsAPng(porytiles::gArgMasterPngPath);

    // Validate master PNG dimensions (must be divisible by 8 to hold tiles)
    png::image<png::rgb_pixel> masterPng{porytiles::gArgMasterPngPath};
    porytiles::validateMasterPngDimensions(masterPng);

    // Master PNG file is safe to tile-ize
    porytiles::verboseLog("--------------- IMPORTING MASTER PNG ---------------");
    porytiles::RgbTiledPng masterTiles{masterPng};

    // Verify that no individual tile in the master has more than 16 colors
    porytiles::validateMasterPngTilesEach16Colors(masterTiles);

    // Verify that the master does not have too many unique colors
    porytiles::validateMasterPngMaxUniqueColors(masterTiles, porytiles::gOptMaxPalettes);

    // Build the tileset and write it out to disk
    porytiles::Tileset tileset{porytiles::gOptMaxPalettes};
    tileset.alignSiblings(masterTiles);
    tileset.buildPalettes(masterTiles);
    tileset.indexTiles(masterTiles);
    tileset.writeTileset();

    return 0;
}
catch (const porytiles::TsException& e) {
    // Catch TsException here, these are errors that can reasonably be expected due to bad input, bad files, etc
    std::cerr << porytiles::errorPrefix() << e.what() << std::endl;
    return 1;
}
catch (const std::exception& e) {
    // TODO : if this catches, something we really didn't expect happened, can we print a stack trace here? How?
    std::cerr << porytiles::fatalPrefix() << e.what() << std::endl;
    std::cerr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
              << std::endl;
    std::cerr
            << "This is a bug. Please file an issue here: https://github.com/grunt-lucas/porytiles/issues"
            << std::endl;
    std::cerr << "Be sure to include the full command you ran, as well as any accompanying input files that"
              << std::endl;
    std::cerr << "trigger the error. This way a maintainer can reproduce the issue." << std::endl;
    return 1;
}
