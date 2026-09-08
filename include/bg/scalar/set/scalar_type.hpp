#pragma once

#include <bg/scalar/set/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class SetScalarType : public ScalarType {
  public:
    VISITOR_SUPPORT()

    virtual const ScalarType &item_type() const noexcept = 0;

    bool supports_equality() const noexcept final { return item_type().supports_equality(); }
    bool supports_hash() const noexcept final { return item_type().supports_hash(); }
};

class GenericSetScalarType final : public SetScalarType {
  public:
    explicit GenericSetScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const ScalarType &item_type() const noexcept override { return *item_type_; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType *item_type_;
};

const SetScalarType &set_scalar_type(const ScalarType &item_type);

template <typename T>
    requires is_set_v<T> && (!details::generic_scalar<T>)
const ScalarType &cpp_scalar_type() {
    return set_scalar_type(details::scalar_type_for<typename T::item_type>());
}

} // namespace bg

namespace bg::details {

template <typename T> struct scalar_type_traits<Set<T>> : scalar_type_properties {
    static constexpr bool is_collection = true;

    static const ScalarType &get() {
        if constexpr (generic_scalar<T>) {
            static const auto type = GenericSetScalarType{scalar_type_for<T>()};

            return type;
        } else {
            return cpp_scalar_type<Set<T>>();
        }
    }
};

template <typename T> struct declaration_variable_traits<Set<T>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<T>(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
