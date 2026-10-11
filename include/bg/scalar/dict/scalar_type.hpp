#pragma once

#include <bg/scalar/dict/type.hpp>
#include <bg/type/details/declaration_variables.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_registry.hpp>

namespace bg {

class DictScalarType : public ScalarType {
  public:
    using generic_scalar_view = Scalar<Dict<SCALAR, SCALAR>>;

    VISITOR_SUPPORT()

    void format_name(std::string &result) const override;
    virtual const ScalarType &key_type() const noexcept = 0;
    virtual const ScalarType &mapped_type() const noexcept = 0;

    bool supports_equality() const noexcept final {
        return key_type().supports_equality() && mapped_type().supports_equality();
    }

    bool supports_hash() const noexcept final { return key_type().supports_hash() && mapped_type().supports_hash(); }

  protected:
    [[nodiscard]] bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

class GenericDictScalarType final : public DictScalarType {
  public:
    GenericDictScalarType(const ScalarType &key_type, const ScalarType &mapped_type) noexcept
        : key_type_(&key_type), mapped_type_(&mapped_type) {}

    bool is_resolved() const noexcept override { return false; }
    [[nodiscard]] const Type &resolve(const TypeMap &type_map) const override;
    const ScalarType &key_type() const noexcept override { return *key_type_; }
    const ScalarType &mapped_type() const noexcept override { return *mapped_type_; }

  private:
    const ScalarType *key_type_;
    const ScalarType *mapped_type_;
};

const DictScalarType &dict_scalar_type(const ScalarType &key_type, const ScalarType &mapped_type);

template <typename K, typename V> struct scalar_type_traits<Dict<K, V>> {
    static const ScalarType &type() {
        if constexpr (scalar_declaration_traits<K>::generic || scalar_declaration_traits<V>::generic) {
            static const auto type =
                GenericDictScalarType{details::scalar_type_for<K>(), details::scalar_type_for<V>()};
            [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);

            return type;
        } else {
            return dict_scalar_type(details::scalar_type_for<K>(), details::scalar_type_for<V>());
        }
    }
};

template <typename K, typename V> struct scalar_declaration_traits<Dict<K, V>> {
    static constexpr bool generic = scalar_declaration_traits<K>::generic || scalar_declaration_traits<V>::generic;
    static constexpr bool is_collection = true;
    static constexpr bool supports_implicit_argument = scalar_declaration_traits<K>::supports_implicit_argument &&
                                                       scalar_declaration_traits<V>::supports_implicit_argument;
    static constexpr bool supports_time_series = scalar_declaration_traits<K>::supports_time_series &&
                                                 scalar_declaration_traits<V>::supports_time_series;
};

} // namespace bg

namespace bg::details {

template <typename K, typename V> struct declaration_variable_traits<Dict<K, V>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        for_each_declaration_variable<K>(visitor);
        for_each_declaration_variable<V>(std::forward<Visitor>(visitor));
    }
};

} // namespace bg::details
