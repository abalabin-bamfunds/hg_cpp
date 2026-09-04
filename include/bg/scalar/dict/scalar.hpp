#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/scalar.hpp>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace bg {

template <typename K, typename V> class Scalar<Dict<K, V>> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept { return runtime_type().item_count(value_.data()); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool contains(const Scalar<K> &key) const {
        return runtime_type().contains(value_.data(), key.input_view());
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        return runtime_type().contains(value_.data(), details::make_scalar_source<K>(value));
    }

    [[nodiscard]] Scalar<V> at(const Scalar<K> &key) const {
        const auto item = runtime_type().at(value_.data(), key.input_view());
        return Scalar<V>::from(item.runtime_type, item.data);
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] Scalar<V> at(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto item = runtime_type().at(value_.data(), details::make_scalar_source<K>(value));
        return Scalar<V>::from(item.runtime_type, item.data);
    }

    template <typename Function> void for_each(Function &&function) const {
        runtime_type().for_each(value_.data(), std::addressof(function),
                                [](void *context, ScalarDictRuntimeType::Entry entry) {
                                    auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
                                    std::invoke(callback, Scalar<K>::from(entry.key.runtime_type, entry.key.data),
                                                Scalar<V>::from(entry.value.runtime_type, entry.value.data));
                                });
    }

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }
    [[nodiscard]] const ScalarType &type() const noexcept { return value_.runtime_type().type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(value_).visit(std::forward<Functions>(functions)...);
    }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_capability_traits<Dict<K, V>>::supports_hash
    {
        return value_.hash();
    }

    [[nodiscard]] bool equals(const details::ScalarData &other) const noexcept
        requires details::scalar_capability_traits<Dict<K, V>>::supports_equality
    {
        return value_.equals(other);
    }

    [[nodiscard]] const details::ScalarData &input_view() const & noexcept { return value_; }
    [[nodiscard]] const details::ScalarData &input_view() const && = delete;

    [[nodiscard]] static Scalar from(const details::ScalarData &value) noexcept { return Scalar{value}; }
    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Dict<K, V>> &value) noexcept {
        return from(cpp_scalar_runtime_type<Dict<K, V>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<Dict<K, V>> &&) = delete;

    using scalar_type = Dict<K, V>;

  private:
    [[nodiscard]] const ScalarDictRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarDictRuntimeType &>(value_.runtime_type());
    }

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
