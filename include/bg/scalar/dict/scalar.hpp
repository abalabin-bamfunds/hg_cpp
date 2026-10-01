#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/scalar.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace bg {

template <typename K, typename V> class Scalar<Dict<K, V>> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t size() const noexcept { return runtime_type().item_count(value_.data()); }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const Scalar<K> &key) const {
        return runtime_type().contains(value_.data(), details::scalar_data(key));
    }

    [[nodiscard]] std::optional<Scalar<V>> find(const Scalar<K> &key) const {
        const auto item = runtime_type().find(value_.data(), details::scalar_data(key));
        if (!item) {
            return std::nullopt;
        }

        return Scalar<V>::from(item->runtime_type, item->data);
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool contains(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        return runtime_type().contains(value_.data(), details::make_scalar_source<K>(value));
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    [[nodiscard]] std::optional<Scalar<V>> find(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
        const auto item = runtime_type().find(value_.data(), details::make_scalar_source<K>(value));
        if (!item) {
            return std::nullopt;
        }

        return Scalar<V>::from(item->runtime_type, item->data);
    }

    Scalar<V> at(const Scalar<K> &key) const {
        const auto item = runtime_type().at(value_.data(), details::scalar_data(key));
        return Scalar<V>::from(item.runtime_type, item.data);
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    Scalar<V> at(Source &&key) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(key));
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

    [[nodiscard]] std::unique_ptr<ScalarDictRuntimeType::Cursor> make_cursor() const {
        return runtime_type().make_cursor(value_.data());
    }

    const ScalarType &type() const noexcept { return value_.runtime_type().type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    std::size_t hash() const noexcept
        requires details::scalar_handle_traits<Dict<K, V>>::supports_hash
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<Dict<K, V>>::supports_equality
    bool equals(const Other &other) const noexcept {
        return value_.equals(details::scalar_data(other));
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
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Dict<K, V>> &value) noexcept {
        return from(cpp_scalar_runtime_type<Dict<K, V>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<Dict<K, V>> &&) = delete;

    using scalar_type = Dict<K, V>;

  public:
    const ScalarDictRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarDictRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
