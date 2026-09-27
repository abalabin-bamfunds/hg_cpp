#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/scalar/struct/runtime_type.hpp>

#include <cstddef>
#include <string_view>
#include <utility>

namespace bg {

template <> class Scalar<STRUCT> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t field_count() const noexcept { return type().field_count(); }
    std::string_view field_name(std::size_t index) const { return type().field(index).name.text(); }
    const ScalarType &field_type(std::size_t index) const { return *type().field(index).type; }

    Scalar<SCALAR> operator[](std::size_t index) const {
        return Scalar<SCALAR>{runtime_type().field(value_, index)};
    }

    const StructScalarType &type() const noexcept { return runtime_type().type(); }
    const StructScalarRuntimeType &runtime_type() const noexcept {
        return static_cast<const StructScalarRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

    std::size_t hash() const noexcept { return value_.hash(); }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); }
    bool equals(const Other &other) const noexcept {
        return value_.equals(details::scalar_data(other));
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }
    template <typename Source> [[nodiscard]] static Scalar from(const Scalar<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const Scalar<Source> &&) = delete;
    template <typename Source> [[nodiscard]] static Scalar from(const ScalarValue<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const ScalarValue<Source> &&) = delete;

    using scalar_type = STRUCT;

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    details::ScalarData value_;
};

} // namespace bg
