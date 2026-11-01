#pragma once

#include <bg/type/size.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/variable.hpp>

namespace bg {

class SizeVariable final : public Variable {
  public:
    using Variable::Variable;

    [[nodiscard]] bool match(std::size_t supplied, TypeMap &type_map) const {
        if (const auto *mapping = type_map.find(*this)) {
            const auto *size = std::get_if<Size>(mapping);
            if (size == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                          "size variable has a non-size mapping"};
            }
            return size->value == supplied;
        }

        type_map.set(*this, Size{supplied});
        return true;
    }

    [[nodiscard]] std::size_t resolve(const TypeMap &type_map) const {
        const auto *mapping = type_map.find(*this);
        if (mapping == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, name(),
                                      "size variable has no type map entry"};
        }

        const auto *size = std::get_if<Size>(mapping);
        if (size == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                      "size variable has a non-size mapping"};
        }
        return size->value;
    }
};

} // namespace bg
