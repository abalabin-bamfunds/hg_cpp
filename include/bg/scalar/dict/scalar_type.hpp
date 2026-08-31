#pragma once

#include <bg/scalar/dict/type.hpp>
#include <bg/type/scalar_type.hpp>

namespace bg {

class DictScalarType : public ScalarType {
  public:
    [[nodiscard]] virtual const ScalarType &key_type() const noexcept = 0;
    [[nodiscard]] virtual const ScalarType &mapped_type() const noexcept = 0;

    [[nodiscard]] bool supports_equality() const noexcept final {
        return key_type().supports_equality() && mapped_type().supports_equality();
    }

    [[nodiscard]] bool supports_hash() const noexcept final {
        return key_type().supports_hash() && mapped_type().supports_hash();
    }
};

class GenericDictScalarType final : public DictScalarType {
  public:
    GenericDictScalarType(const ScalarType &key_type, const ScalarType &mapped_type) noexcept
        : key_type_(&key_type), mapped_type_(&mapped_type) {}

    [[nodiscard]] bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] bool match(const Type &supplied, TypeMap &bindings) const override;
    [[nodiscard]] const Type &resolve(const TypeMap &bindings) const override;
    [[nodiscard]] const ScalarType &key_type() const noexcept override { return *key_type_; }
    [[nodiscard]] const ScalarType &mapped_type() const noexcept override { return *mapped_type_; }

  private:
    const ScalarType *key_type_;
    const ScalarType *mapped_type_;
};

[[nodiscard]] const DictScalarType &dict_scalar_type(const ScalarType &key_type, const ScalarType &mapped_type);

template <typename T>
    requires is_dict_v<T>
const ScalarType &cpp_scalar_type() {
    return dict_scalar_type(cpp_scalar_type<typename T::key_type>(), cpp_scalar_type<typename T::mapped_type>());
}

} // namespace bg

namespace bg::details {

template <typename K, typename V> struct scalar_type_traits<Dict<K, V>> : scalar_type_properties {
    static constexpr bool is_collection = true;

    template <typename Scope> [[nodiscard]] static const ScalarType &get() {
        if constexpr (generic_scalar<K> || generic_scalar<V>) {
            static const auto type = GenericDictScalarType{scalar_type_for<Scope, K>(), scalar_type_for<Scope, V>()};

            return type;
        } else {
            return cpp_scalar_type<Dict<K, V>>();
        }
    }
};

} // namespace bg::details
