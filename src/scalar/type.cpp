#include <bg/scalar/details/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/util/symbol.hpp>

namespace bg {
namespace {

template <typename T> class CppScalarType final : public ScalarType {
  public:
    [[nodiscard]] bool is_resolved() const noexcept override { return true; }

    [[nodiscard]] const Type &resolve(const TypeMap &) const override { return *this; }

    [[nodiscard]] bool supports_equality() const noexcept override {
        return details::scalar_storage_traits<T>::supports_equality;
    }

    [[nodiscard]] bool supports_hash() const noexcept override {
        return details::scalar_storage_traits<T>::supports_hash;
    }
};

} // namespace

const ScalarType &int_type() noexcept {
    static const CppScalarType<int> type;

    return type;
}

const ScalarType &bool_type() noexcept {
    static const CppScalarType<bool> type;

    return type;
}

const ScalarType &int64_type() noexcept {
    static const CppScalarType<std::int64_t> type;

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
    static const CppScalarType<EvaluationTime> type;

    return type;
}

const ScalarType &string_type() noexcept {
    static const CppScalarType<string> type;

    return type;
}

const ScalarType &symbol_type() noexcept {
    static const CppScalarType<util::symbol> type;

    return type;
}

template <> const ScalarType &cpp_scalar_type<int>() noexcept {
    return int_type();
}

template <> const ScalarType &cpp_scalar_type<bool>() noexcept {
    return bool_type();
}

template <> const ScalarType &cpp_scalar_type<std::int64_t>() noexcept {
    return int64_type();
}

template <> const ScalarType &cpp_scalar_type<float>() noexcept {
    return float_type();
}

template <> const ScalarType &cpp_scalar_type<double>() noexcept {
    return double_type();
}

template <> const ScalarType &cpp_scalar_type<EvaluationTime>() noexcept {
    return evaluation_time_type();
}

template <> const ScalarType &cpp_scalar_type<string>() noexcept {
    return string_type();
}

template <> const ScalarType &cpp_scalar_type<util::symbol>() noexcept {
    return symbol_type();
}

} // namespace bg
