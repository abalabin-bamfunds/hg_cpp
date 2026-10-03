#pragma once

#include <bg/scalar/set/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class SetScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<Set<SCALAR>>;

    VISITOR_SUPPORT()

    virtual const ScalarType &item_type() const noexcept = 0;

    bool supports_equality() const noexcept final { return item_type().supports_equality(); }
    bool supports_hash() const noexcept final { return item_type().supports_hash(); }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

class GenericSetScalarType final : public SetScalarType {
  public:
    explicit GenericSetScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

const SetScalarType &set_scalar_type(const ScalarType &item_type);

template <typename T> struct scalar_type_traits<Set<T>> {
    static const ScalarType &type() {
        if constexpr (scalar_declaration_traits<T>::generic) {
            static const auto type = GenericSetScalarType{details::scalar_type_for<T>()};

            return type;
        } else {
            return set_scalar_type(details::scalar_type_for<T>());
        }
    }
};

template <typename T> struct scalar_declaration_traits<Set<T>> {
    static constexpr bool generic = scalar_declaration_traits<T>::generic;
    static constexpr bool is_collection = true;
    static constexpr bool supports_implicit_argument = scalar_declaration_traits<T>::supports_implicit_argument;
    static constexpr bool supports_time_series = scalar_declaration_traits<T>::supports_time_series;
};

} // namespace bg

namespace bg::details {

template <typename T> struct declaration_variable_traits<Set<T>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<T>(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
