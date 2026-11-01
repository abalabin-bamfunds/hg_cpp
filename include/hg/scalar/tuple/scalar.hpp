#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/scalar/tuple/runtime_type.hpp>
#include <bg/scalar/tuple/storage_traits.hpp>

#include <cstddef>
#include <tuple>
#include <utility>

namespace bg {

template <typename... Items> class Scalar<Tuple<Items...>> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    template <std::size_t Index> auto get() const -> Scalar<typename Tuple<Items...>::template item_type<Index>> {
        static_assert(Index < sizeof...(Items));
        const auto item = Scalar<>::from(runtime_type().item(value_, Index));
        return Scalar<typename Tuple<Items...>::template item_type<Index>>::from(item);
    }

    const TupleScalarType &type() const noexcept { return runtime_type().type(); }
    const ScalarTupleRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarTupleRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

    std::size_t hash() const
        requires details::scalar_handle_traits<Tuple<Items...>>::supports_hash
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<Tuple<Items...>>::supports_equality
    bool equals(const Other &other) const {
        return value_.equals(details::scalar_data(other));
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{runtime_type.borrow(data)};
    }
    template <typename Source> [[nodiscard]] static Scalar from(const Scalar<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const Scalar<Source> &&) = delete;
    template <typename Source> [[nodiscard]] static Scalar from(const ScalarValue<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const ScalarValue<Source> &&) = delete;

    using scalar_type = Tuple<Items...>;

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    details::ScalarData value_;
};

template <> class Scalar<TUPLE> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t size() const noexcept { return runtime_type().type().size(); }
    const ScalarType &item_type(std::size_t index) const { return runtime_type().type().item_type(index); }
    Scalar<SCALAR> operator[](std::size_t index) const {
        const auto item_value = Scalar<>::from(runtime_type().item(value_, index));
        return Scalar<SCALAR>::from(item_value);
    }

    const TupleScalarType &type() const noexcept { return runtime_type().type(); }
    const ScalarTupleRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarTupleRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{runtime_type.borrow(data)};
    }
    template <typename Source> [[nodiscard]] static Scalar from(const Scalar<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const Scalar<Source> &&) = delete;
    template <typename Source> [[nodiscard]] static Scalar from(const ScalarValue<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const ScalarValue<Source> &&) = delete;

    using scalar_type = TUPLE;

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    details::ScalarData value_;
};

} // namespace bg

namespace bg {

template <typename... Items>
auto scalar_storage_traits<Tuple<Items...>>::read(const stored_type &value) -> value_type {
    return value_type::from(cpp_scalar_runtime_type<Tuple<Items...>>(), &value);
}

template <typename... Items>
auto scalar_storage_traits<Tuple<Items...>>::read_value(const ScalarData &value) -> value_type {
    const auto scalar = Scalar<>::from(value);
    return value_type::from(scalar);
}

template <typename... Items> std::size_t scalar_storage_traits<Tuple<Items...>>::hash(value_type value) {
    return details::scalar_data(value).hash();
}

template <typename... Items>
bool scalar_storage_traits<Tuple<Items...>>::equals(value_type left, value_type right) {
    return details::scalar_data(left).equals(details::scalar_data(right));
}

template <typename... Items>
void scalar_storage_traits<Tuple<Items...>>::construct(stored_type *destination, value_type value, allocator &memory) {
    details::scalar_data(value).copy_construct(destination, cpp_scalar_runtime_type<Tuple<Items...>>(), memory);
}

template <typename... Items>
void scalar_storage_traits<Tuple<Items...>>::copy_construct(stored_type *destination, const stored_type &source,
                                                           allocator &memory) {
    cpp_scalar_runtime_type<Tuple<Items...>>().copy_construct(destination, &source, memory);
}

template <typename... Items>
void scalar_storage_traits<Tuple<Items...>>::assign(stored_type &destination, value_type value, allocator &) {
    details::scalar_data(value).assign(&destination, cpp_scalar_runtime_type<Tuple<Items...>>());
}

template <typename... Items>
void scalar_storage_traits<Tuple<Items...>>::destroy(stored_type *value, allocator &memory) noexcept {
    cpp_scalar_runtime_type<Tuple<Items...>>().destroy(value, memory);
}

} // namespace bg
