#pragma once

#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/bundle/storage_traits.hpp>
#include <bg/scalar/scalar.hpp>

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bg {

template <typename Declaration>
    requires details::hierarchical_scalar_declaration<Declaration> || std::same_as<Declaration, HIERARCHICAL>
class Scalar<Declaration> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t field_count() const noexcept { return type().field_count(); }
    std::string_view field_name(std::size_t index) const { return type().field(index).name.text(); }
    const ScalarType &field_type(std::size_t index) const {
        return static_cast<const ScalarType &>(*type().field(index).type);
    }
    bool present(std::size_t index) const {
        check_index(index);
        return runtime_type().field_present(value_, index);
    }
    Scalar<SCALAR> operator[](std::size_t index) const {
        check_index(index);
        return Scalar<SCALAR>{runtime_type().field(value_, index)};
    }

    const ResolvedHierarchicalScalarType &type() const noexcept { return runtime_type().type(); }
    const HierarchicalScalarRuntimeType &runtime_type() const noexcept {
        return static_cast<const HierarchicalScalarRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

    std::size_t hash() const noexcept
        requires details::scalar_handle_traits<Declaration>::supports_hash
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<Declaration>::supports_equality
    bool equals(const Other &other) const noexcept {
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

    using scalar_type = Declaration;

  private:
    template <typename T> friend const details::ScalarData &details::scalar_data(const Scalar<T> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    void check_index(std::size_t index) const {
        if (index >= field_count()) {
            throw std::out_of_range{"hierarchical scalar field index"};
        }
    }

    details::ScalarData value_;
};

} // namespace bg
