#pragma once

#include <bg/scalar/bundle/runtime_type.hpp>
#include <bg/scalar/scalar.hpp>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bg::details {

template <typename... T> struct hierarchical_capability_stack {};

template <typename T, typename Stack, bool Hash> struct hierarchical_capability {
    static constexpr bool value =
        Hash ? scalar_handle_traits<T>::supports_hash : scalar_handle_traits<T>::supports_equality;
};

template <typename T, typename... Seen, bool Hash>
struct hierarchical_capability<List<T>, hierarchical_capability_stack<Seen...>, Hash>
    : hierarchical_capability<T, hierarchical_capability_stack<Seen...>, Hash> {};

template <typename T, typename... Seen, bool Hash>
struct hierarchical_capability<Set<T>, hierarchical_capability_stack<Seen...>, Hash>
    : hierarchical_capability<T, hierarchical_capability_stack<Seen...>, Hash> {};

template <typename K, typename V, typename... Seen, bool Hash>
struct hierarchical_capability<Dict<K, V>, hierarchical_capability_stack<Seen...>, Hash> {
    static constexpr bool value = hierarchical_capability<K, hierarchical_capability_stack<Seen...>, Hash>::value &&
                                  hierarchical_capability<V, hierarchical_capability_stack<Seen...>, Hash>::value;
};

template <template <template <typename, auto...> typename> typename Schema, typename... Seen, bool Hash>
struct hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<Seen...>, Hash> {
  private:
    using Declaration = Bundle<Schema>;
    using Reflection = hierarchical_scalar_schema_reflection<Schema>;
    using Next = hierarchical_capability_stack<Seen..., Declaration>;

    template <std::size_t... Index> static consteval bool direct_fields(std::index_sequence<Index...>) {
        return (hierarchical_capability<typename Reflection::template direct_field<Index>::scalar_type, Next,
                                        Hash>::value &&
                ...);
    }

    static consteval bool calculate() {
        if constexpr ((std::same_as<Declaration, Seen> || ...)) {
            return true;
        } else {
            auto result = direct_fields(std::make_index_sequence<Reflection::direct_field_count>{});
            if constexpr (Reflection::has_base) {
                using Base = typename Reflection::base_schema;
                result = result && hierarchical_capability<Bundle<Base::template apply>, Next, Hash>::value;
            }
            return result;
        }
    }

  public:
    static constexpr bool value = calculate();
};

template <template <template <typename, auto...> typename> typename Schema>
struct scalar_handle_traits<Bundle<Schema>> {
    static constexpr bool supports_equality =
        hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<>, false>::value;
    static constexpr bool supports_hash =
        hierarchical_capability<Bundle<Schema>, hierarchical_capability_stack<>, true>::value;
};

} // namespace bg::details

namespace bg {

template <template <template <typename, auto...> typename> typename Schema> class Scalar<Bundle<Schema>> {
    using Declaration = Bundle<Schema>;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t field_count() const noexcept { return declared_type().field_count(); }
    std::string_view field_name(std::size_t index) const { return declared_type().field(index).name.text(); }
    const ScalarType &field_type(std::size_t index) const { return *declared_type().field(index).type; }
    bool present(std::size_t index) const {
        check_index(index);
        return runtime_type().field_present(value_, index);
    }
    Scalar<SCALAR> operator[](std::size_t index) const {
        check_index(index);
        return Scalar<SCALAR>{runtime_type().field(value_, index)};
    }

    const HierarchicalScalarType &type() const noexcept { return runtime_type().type(); }
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
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}

    static const HierarchicalScalarType &declared_type() noexcept {
        return static_cast<const HierarchicalScalarType &>(cpp_scalar_type<Declaration>());
    }

    void check_index(std::size_t index) const {
        if (index >= field_count()) {
            throw std::out_of_range{"hierarchical scalar field index"};
        }
    }

    details::ScalarData value_;
};

} // namespace bg
