#pragma once

#include <stdexcept>

namespace bg {

class WiringError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace bg
