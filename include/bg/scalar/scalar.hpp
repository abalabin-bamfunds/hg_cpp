#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/capability_traits.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

#include <concepts>
#include <memory>
#include <type_traits>

namespace bg {

template <typename T = void> class Scalar;
template <typename T = void> class ScalarValue;

template <typename T> class Scalar {
    static constexpr auto variable = details::scalar_variable<T>;
    static constexpr auto erased = std::same_as<T, void> || variable;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }

    [[nodiscard]] details::scalar_value_t<T> value() const
        requires(!erased)
    {
        return details::scalar_data_value<T>(value_);
    }

    [[nodiscard]] const ScalarType &type() const noexcept { return value_.runtime_type().type(); }
    [[nodiscard]] const ScalarRuntimeType &runtime_type() const noexcept { return value_.runtime_type(); }
    [[nodiscard]] const void *data() const noexcept { return value_.data(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires std::same_as<T, void> || details::scalar_capability_traits<T>::supports_hash
    {
        return value_.hash();
    }

    [[nodiscard]] bool equals(const details::ScalarData &other) const noexcept
        requires std::same_as<T, void> || details::scalar_capability_traits<T>::supports_equality
    {
        return value_.equals(other);
    }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit(Visitor &&visitor, Arguments &&...arguments) const;

    template <typename Function, typename... Functions>
    decltype(auto) visit(Function &&function, Functions &&...functions) const {
        return visit(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    [[nodiscard]] const details::ScalarData &input_view() const & noexcept { return value_; }
    [[nodiscard]] const details::ScalarData &input_view() const && = delete;

    using scalar_type = T;

    template <typename Value = T>
        requires(!erased && std::same_as<Value, T>)
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Value> &value) noexcept {
        return Scalar{details::ScalarData{cpp_scalar_runtime_type<Value>(), std::addressof(value)}};
    }

    template <typename Value = T>
        requires(!erased && std::same_as<Value, T>)
    static Scalar from(details::scalar_stored_t<Value> &&) = delete;

    template <typename Value>
        requires erased
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Value> &value) noexcept {
        return Scalar{details::ScalarData{cpp_scalar_runtime_type<Value>(), std::addressof(value)}};
    }

    template <typename Value>
        requires erased
    static Scalar from(details::scalar_stored_t<Value> &&) = delete;

    [[nodiscard]] static Scalar from(const details::ScalarData &value) noexcept { return Scalar{value}; }

    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }

  private:
    template <typename> friend class ScalarValue;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    details::ScalarData value_;
};

} // namespace bg

#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/set/scalar.hpp>

namespace bg {

using ScalarVisitTypes = ddv::mux<Scalar<List<SCALAR>>, Scalar<Set<SCALAR>>, Scalar<Dict<SCALAR, SCALAR>>, Scalar<>>;

template <typename T>
template <typename Visitor, typename... Arguments>
    requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
auto Scalar<T>::visit(Visitor &&visitor, Arguments &&...arguments) const {
    auto dispatch = ddv::make_visitable_visitor<ScalarVisitTypes>(
        std::forward_as_tuple(std::forward<Arguments>(arguments)...), std::forward<Visitor>(visitor));
    auto &types = static_cast<ScalarVisitTypes &>(dispatch);
    type().visit(
        [&](const ListScalarType *) {
            auto typed = Scalar<List<SCALAR>>::from(value_);
            types.visit(&typed);
        },
        [&](const SetScalarType *) {
            auto typed = Scalar<Set<SCALAR>>::from(value_);
            types.visit(&typed);
        },
        [&](const DictScalarType *) {
            auto typed = Scalar<Dict<SCALAR, SCALAR>>::from(value_);
            types.visit(&typed);
        },
        [&](const ScalarType *) {
            auto typed = Scalar<>::from(value_);
            types.visit(&typed);
        },
        ddv::noop);
    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

static_assert(sizeof(Scalar<int>) == 2 * sizeof(void *));

} // namespace bg
