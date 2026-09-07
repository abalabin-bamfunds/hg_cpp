#pragma once

#include <bg/util/symbol.hpp>

namespace bg {

class Variable {
  public:
    explicit Variable(util::symbol name) : name_(name) {}
    virtual ~Variable();

    util::symbol name() const noexcept { return name_; }

  private:
    util::symbol name_;
};

} // namespace bg
