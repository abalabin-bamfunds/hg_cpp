#include <bg/scalar/details/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/symbol.hpp>

#include <type_traits>

namespace bg {
namespace {

template <typename T>
using CppScalarTypeBase = std::conditional_t<tp::contains<T>(details::builtin_scalar_declarations), BuiltinScalarType,
                                             ScalarType>;

template <typename T> class CppScalarType final : public CppScalarTypeBase<T> {
    using Base = CppScalarTypeBase<T>;

  public:
    CppScalarType() requires std::same_as<Base, BuiltinScalarType> : Base{tp::unit<T>{}} {}
    CppScalarType() requires std::same_as<Base, ScalarType> = default;

    bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }

    void format_name(std::string &result) const override {
        if constexpr (std::same_as<T, int>) {
            result += "int";
        } else if constexpr (std::same_as<T, byte>) {
            result += "byte";
        } else if constexpr (std::same_as<T, bool>) {
            result += "bool";
        } else if constexpr (std::same_as<T, i64>) {
            result += "i64";
        } else if constexpr (std::same_as<T, float>) {
            result += "float";
        } else if constexpr (std::same_as<T, double>) {
            result += "double";
        } else if constexpr (std::same_as<T, date_time_t>) {
            result += "date_time_t";
        } else if constexpr (std::same_as<T, timedelta_t>) {
            result += "timedelta_t";
        } else if constexpr (std::same_as<T, string>) {
            result += "string";
        } else if constexpr (std::same_as<T, symbol>) {
            result += "symbol";
        } else {
            ScalarType::format_name(result);
        }
    }

    bool supports_equality() const noexcept override { return scalar_storage_traits<T>::supports_equality; }

    bool supports_hash() const noexcept override { return scalar_storage_traits<T>::supports_hash; }
};

template <typename T> const CppScalarType<T> &registered_cpp_scalar_type() noexcept {
    static const CppScalarType<T> type;
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
    return type;
}

} // namespace

const ScalarType &int_type() noexcept {
    return registered_cpp_scalar_type<int>();
}

const ScalarType &byte_type() noexcept {
    return registered_cpp_scalar_type<byte>();
}

const ScalarType &bool_type() noexcept {
    return registered_cpp_scalar_type<bool>();
}

const ScalarType &int64_type() noexcept {
    return registered_cpp_scalar_type<i64>();
}

const ScalarType &float_type() noexcept {
    return registered_cpp_scalar_type<float>();
}

const ScalarType &double_type() noexcept {
    return registered_cpp_scalar_type<double>();
}

const ScalarType &evaluation_time_type() noexcept {
    return registered_cpp_scalar_type<date_time_t>();
}

const ScalarType &timedelta_type() noexcept {
    return registered_cpp_scalar_type<timedelta_t>();
}

const ScalarType &string_type() noexcept {
    return registered_cpp_scalar_type<string>();
}

const ScalarType &symbol_type() noexcept {
    return registered_cpp_scalar_type<symbol>();
}

const ScalarType &scalar_type_traits<int>::type() noexcept {
    return int_type();
}

const ScalarType &scalar_type_traits<bool>::type() noexcept {
    return bool_type();
}

const ScalarType &scalar_type_traits<byte>::type() noexcept {
    return byte_type();
}

const ScalarType &scalar_type_traits<i64>::type() noexcept {
    return int64_type();
}

const ScalarType &scalar_type_traits<float>::type() noexcept {
    return float_type();
}

const ScalarType &scalar_type_traits<double>::type() noexcept {
    return double_type();
}

const ScalarType &scalar_type_traits<date_time_t>::type() noexcept {
    return evaluation_time_type();
}

const ScalarType &scalar_type_traits<timedelta_t>::type() noexcept {
    return timedelta_type();
}

const ScalarType &scalar_type_traits<string>::type() noexcept {
    return string_type();
}

const ScalarType &scalar_type_traits<symbol>::type() noexcept {
    return symbol_type();
}

} // namespace bg
