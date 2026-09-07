#pragma once

#include <bg/util/symbol.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace bg {

enum class TypeResolutionErrorCode {
    argument_count,
    mismatch,
    ambiguous_constraint,
    unbound_variable,
    invalid_binding,
};

class TypeResolutionError : public std::runtime_error {
  public:
    TypeResolutionError(TypeResolutionErrorCode code, util::symbol subject, std::string message)
        : std::runtime_error(std::move(message)), code_(code), subject_(subject) {}

    TypeResolutionErrorCode code() const noexcept { return code_; }

    util::symbol subject() const noexcept { return subject_; }

  private:
    TypeResolutionErrorCode code_;
    util::symbol subject_;
};

} // namespace bg
