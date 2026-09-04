#pragma once

#include <bg/type/size.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/variable.hpp>

namespace bg {

class SizeVariable final : public Variable {
  public:
    using Variable::Variable;

    [[nodiscard]] bool match(std::size_t supplied, TypeMap &bindings) const {
        if (const auto *binding = bindings.find(*this)) {
            const auto *size = std::get_if<Size>(binding);
            if (size == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                          "size variable has a non-size binding"};
            }
            return size->value == supplied;
        }

        bindings.set(*this, Size{supplied});
        return true;
    }

    [[nodiscard]] std::size_t resolve(const TypeMap &bindings) const {
        const auto *binding = bindings.find(*this);
        if (binding == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unbound_variable, name(), "size variable is unbound"};
        }

        const auto *size = std::get_if<Size>(binding);
        if (size == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                      "size variable has a non-size binding"};
        }
        return size->value;
    }
};

} // namespace bg
