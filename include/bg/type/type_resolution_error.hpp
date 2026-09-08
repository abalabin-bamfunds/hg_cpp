#pragma once

#include <bg/util/symbol.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace bg {

class Type;

enum class TypeResolutionErrorCode {
    argument_count,
    mismatch,
    ambiguous_constraint,
    unmapped_variable,
    invalid_type_map,
};

class TypeResolutionError : public std::runtime_error {
  public:
    TypeResolutionError(TypeResolutionErrorCode code, util::symbol subject, std::string message,
                        const Type *expected = nullptr, const Type *supplied = nullptr)
        : std::runtime_error(std::move(message)), code_(code), subject_(subject), expected_(expected),
          supplied_(supplied) {}

    TypeResolutionErrorCode code() const noexcept { return code_; }

    util::symbol subject() const noexcept { return subject_; }

    const Type *expected() const noexcept { return expected_; }
    const Type *supplied() const noexcept { return supplied_; }

  private:
    TypeResolutionErrorCode code_;
    util::symbol subject_;
    const Type *expected_;
    const Type *supplied_;
};

} // namespace bg
