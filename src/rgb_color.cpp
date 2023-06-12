#include "rgb_color.h"

#include <string>

namespace porytiles {
bool RgbColor::operator==(const RgbColor& other) const {
    return red == other.red && green == other.green && blue == other.blue;
}

bool RgbColor::operator!=(const RgbColor& other) const {
    return !(*this == other);
}

std::string RgbColor::prettyString() const {
    return std::to_string(red) + "," + std::to_string(green) + "," + std::to_string(blue);
}

std::string RgbColor::jascString() const {
    return std::to_string(red) + " " + std::to_string(green) + " " + std::to_string(blue);
}
} // namespace porytiles