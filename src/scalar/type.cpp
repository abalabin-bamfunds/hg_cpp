#include <bg/scalar/details/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
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

    bool supports_equality() const noexcept override { return scalar_storage_traits<T>::supports_equality; }

    bool supports_hash() const noexcept override { return scalar_storage_traits<T>::supports_hash; }
};

} // namespace

const ScalarType &int_type() noexcept {
    static const CppScalarType<int> type;

    return type;
}

const ScalarType &byte_type() noexcept {
    static const CppScalarType<byte> type;

    return type;
}

const ScalarType &bool_type() noexcept {
    static const CppScalarType<bool> type;

    return type;
}

const ScalarType &int64_type() noexcept {
    static const CppScalarType<i64> type;

    return type;
}

const ScalarType &float_type() noexcept {
    static const CppScalarType<float> type;

    return type;
}

const ScalarType &double_type() noexcept {
    static const CppScalarType<double> type;

    return type;
}

const ScalarType &evaluation_time_type() noexcept {
    static const CppScalarType<date_time_t> type;

    return type;
}

const ScalarType &timedelta_type() noexcept {
    static const CppScalarType<timedelta_t> type;

    return type;
}

const ScalarType &string_type() noexcept {
    static const CppScalarType<string> type;

    return type;
}

const ScalarType &symbol_type() noexcept {
    static const CppScalarType<symbol> type;

    return type;
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
