#pragma once

#include <bg/scalar/list/type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

namespace bg {

class ListScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<List<SCALAR>>;

    VISITOR_SUPPORT()

    void format_name(std::string &result) const override;
    virtual const ScalarType &item_type() const noexcept = 0;

    bool supports_equality() const noexcept final { return item_type().supports_equality(); }
    bool supports_hash() const noexcept final { return item_type().supports_hash(); }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

class GenericListScalarType final : public ListScalarType {
  public:
    explicit GenericListScalarType(const ScalarType &item_type) noexcept : item_type_(&item_type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const ScalarType &item_type() const noexcept override { return *item_type_; }

  private:
    const ScalarType *item_type_;
};

const ListScalarType &list_scalar_type(const ScalarType &item_type);

template <typename T> struct scalar_type_traits<List<T>> {
    static const ScalarType &type() {
        if constexpr (scalar_declaration_traits<T>::generic) {
            static const auto type = util::static_storage<GenericListScalarType>{details::scalar_type_for<T>()};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());

            return type.get();
        } else {
            const auto &type = list_scalar_type(details::scalar_type_for<T>());
            type.set_default_runtime_type_factory(&details::cpp_scalar_runtime_type_for_type<List<T>>);
            return type;
        }
    }
};

template <typename T> struct scalar_declaration_traits<List<T>> {
    static constexpr bool generic = scalar_declaration_traits<T>::generic;
    static constexpr bool is_collection = true;
    static constexpr bool supports_implicit_argument = scalar_declaration_traits<T>::supports_implicit_argument;
    static constexpr bool supports_time_series = scalar_declaration_traits<T>::supports_time_series;
};

} // namespace bg

namespace bg::details {

template <typename T> struct declaration_variable_traits<List<T>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<T>(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details

#include <bg/scalar/cpp_runtime_type.hpp>
