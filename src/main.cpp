#include "png_checks.h"
#include "cli_parser.h"
#include "tsexception.h"
#include "rgb_tiled_png.h"

#include <iostream>
#include <png.hpp>

namespace tscreate {
std::string errorPrefix() {
    std::string program(PROGRAM_NAME);
    return program + ": error: ";
}

std::string fatalPrefix() {
    std::string program(PROGRAM_NAME);
    return program + ": fatal: ";
}
} // namespace tscreate

int main(int argc, char** argv) try {
    // Parse CLI options and args, fills out global opt vars with expected values
    tscreate::parseOptions(argc, argv);

    // Verifies that master PNG path points at a valid PNG file
    tscreate::validateMasterPngIsAPng(tscreate::gArgMasterPngPath);

    png::image<png::rgb_pixel> masterPng(tscreate::gArgMasterPngPath);

    // Verifies that master PNG exists and validates its dimensions (must be divisible by 8 to hold tiles)
    tscreate::validateMasterPngDimensions(masterPng);

    // Master PNG file is safe to tile-ize
    tscreate::RgbTiledPng masterTiles{masterPng};

    // Verifies that no individual tile in the master has more than 16 colors
    tscreate::validateMasterPngTilesEach16Colors(masterTiles);

    // Verifies that the master does not have too many unique colors
    tscreate::validateMasterPngMaxUniqueColors(masterTiles);

    // Create output directory if possible, otherwise fail
    // std::filesystem::create_directory(tscreate::gArgOutputPath);

    // TODO : remove this test code that simply writes the master PNG to output dir
    // std::filesystem::path parentDirectory(tscreate::gArgOutputPath);
    // std::filesystem::path outputFile("output.png");
    // png::image<png::rgb_pixel> masterPng(tscreate::gArgMasterPngPath);
    // masterPng.write(parentDirectory / outputFile);

    return 0;
}
catch (tscreate::TsException& e) {
    // Catch TsException here, these are errors that can reasonably be expected due to bad input, bad files, etc
    std::cerr << tscreate::errorPrefix() << e.what() << std::endl;
    return 1;
}
catch (std::exception& e) {
    // TODO : if this catches, something we really didn't expect happened, can we print a stack trace here? How?
    std::cerr << tscreate::fatalPrefix() << e.what() << std::endl;
    std::cerr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"
              << std::endl;
    std::cerr
            << "This is a bug. Please file an issue here: https://github.com/grunt-lucas/tscreate/issues"
            << std::endl;
    std::cerr << "Be sure to include the full command you ran, as well as any accompanying input files that"
              << std::endl;
    std::cerr << "trigger the error. This way a maintainer can reproduce the issue." << std::endl;
    return 1;
}
