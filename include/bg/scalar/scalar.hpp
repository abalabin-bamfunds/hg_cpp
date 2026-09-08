#pragma once

#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/capability_traits.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/details/value_access.hpp>
#include <bg/scalar/type.hpp>

#include <ddv/serial_visitor.h>
#include <ddv/visitable.h>

#include <concepts>
#include <memory>
#include <type_traits>

namespace bg {

template <typename T> class Scalar;
template <typename T = void> class ScalarValue;

namespace details {

template <typename T, typename Stored> [[nodiscard]] consteval bool concrete_scalar_storage_matches() {
    if constexpr (std::same_as<T, void> || scalar_variable<T>) {
        return false;
    } else {
        return std::same_as<std::remove_cvref_t<Stored>, scalar_stored_t<T>>;
    }
}

} // namespace details

template <typename T> class Scalar {
    template <typename U> [[nodiscard]] static consteval bool supports_hash_for() {
        if constexpr (std::same_as<U, void>) {
            return true;
        } else {
            return details::scalar_capability_traits<U>::supports_hash;
        }
    }

    template <typename U> [[nodiscard]] static consteval bool supports_equality_for() {
        if constexpr (std::same_as<U, void>) {
            return true;
        } else {
            return details::scalar_capability_traits<U>::supports_equality;
        }
    }

    static constexpr auto variable = details::scalar_variable<T>;
    static constexpr auto erased = std::same_as<T, void> || variable;
    static constexpr auto supports_hash = supports_hash_for<T>();
    static constexpr auto supports_equality = supports_equality_for<T>();

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    details::scalar_value_t<T> value() const
        requires(!erased)
    {
        return details::scalar_data_value<T>(value_);
    }

    details::scalar_value_t<T> operator*() const
        requires(!erased)
    {
        return value();
    }

    auto operator->() const
        requires(!erased && details::scalar_value_supports_arrow<T>)
    {
        return details::scalar_value_arrow<T>(value());
    }

    const ScalarType &type() const noexcept { return value_.runtime_type().type(); }
    const ScalarRuntimeType &runtime_type() const noexcept { return value_.runtime_type(); }
    const void *data() const noexcept { return value_.data(); }

    std::size_t hash() const noexcept
        requires supports_hash
    {
        return value_.hash();
    }

    bool equals(const details::ScalarData &other) const noexcept
        requires supports_equality
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

    operator const details::ScalarData &() const & noexcept { return value_; }
    operator const details::ScalarData &() const && = delete;

    using scalar_type = T;

    template <typename Stored>
        requires(details::concrete_scalar_storage_matches<T, Stored>())
    [[nodiscard]] static Scalar from(const Stored &value) noexcept {
        return Scalar{details::ScalarData{cpp_scalar_runtime_type<T>(), std::addressof(value)}};
    }

    template <typename Stored>
        requires(details::concrete_scalar_storage_matches<T, Stored>())
    static Scalar from(const Stored &&) = delete;

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
        [&]<typename RuntimeType>(const RuntimeType *) {
            if constexpr (requires { typename RuntimeType::generic_scalar_view; }) {
                using View = typename RuntimeType::generic_scalar_view;
                auto typed = View::from(value_);
                types.visit(&typed);
            }
        },
        ddv::noop);
    if constexpr (!decltype(dispatch)::is_result_void) {
        return *dispatch;
    }
}

static_assert(sizeof(Scalar<int>) == 2 * sizeof(void *));

} // namespace bg
