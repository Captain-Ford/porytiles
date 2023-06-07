#ifndef TSCREATE_TSEXCEPTION_H
#define TSCREATE_TSEXCEPTION_H

#include <string>
#include <stdexcept>

namespace tscreate {
// Generic tscreate exception class
class TsException : public std::runtime_error {
public:
    explicit TsException(const std::string& msg) : std::runtime_error{msg} {}
};
} // namespace tscreate

#endif // TSCREATE_TSEXCEPTION_H