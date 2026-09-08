#pragma once

#include <bg/scalar/dict/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class DictScalarType : public ScalarType {
  public:
    VISITOR_SUPPORT()

    virtual const ScalarType &key_type() const noexcept = 0;
    virtual const ScalarType &mapped_type() const noexcept = 0;

    bool supports_equality() const noexcept final {
        return key_type().supports_equality() && mapped_type().supports_equality();
    }

    bool supports_hash() const noexcept final { return key_type().supports_hash() && mapped_type().supports_hash(); }
};

class GenericDictScalarType final : public DictScalarType {
  public:
    GenericDictScalarType(const ScalarType &key_type, const ScalarType &mapped_type) noexcept
        : key_type_(&key_type), mapped_type_(&mapped_type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const ScalarType &key_type() const noexcept override { return *key_type_; }
    const ScalarType &mapped_type() const noexcept override { return *mapped_type_; }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType *key_type_;
    const ScalarType *mapped_type_;
};

const DictScalarType &dict_scalar_type(const ScalarType &key_type, const ScalarType &mapped_type);

template <typename T>
    requires is_dict_v<T> && (!details::generic_scalar<T>)
const ScalarType &cpp_scalar_type() {
    return dict_scalar_type(details::scalar_type_for<typename T::key_type>(),
                            details::scalar_type_for<typename T::mapped_type>());
}

} // namespace bg

namespace bg::details {

template <typename K, typename V> struct scalar_type_traits<Dict<K, V>> : scalar_type_properties {
    static constexpr bool is_collection = true;

    static const ScalarType &get() {
        if constexpr (generic_scalar<K> || generic_scalar<V>) {
            static const auto type = GenericDictScalarType{scalar_type_for<K>(), scalar_type_for<V>()};

            return type;
        } else {
            return cpp_scalar_type<Dict<K, V>>();
        }
    }
};

template <typename K, typename V> struct declaration_variable_traits<Dict<K, V>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<K>(visitor);
        for_each_declaration_variable<V>(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
