#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/mutable_view.hpp>
#include <bg/scalar/type.hpp>
#include <bg/scalar/view.hpp>
#include <bg/type/type_map.hpp>
#include <bg/util/polymorphic_value.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace bg {

template <typename T = void> class Scalar;
template <typename T = void> class ScalarValue;

namespace details {

using ErasedScalarView = util::polymorphic_value<ScalarValueInputView>;

template <typename T> using TypedScalarView = util::polymorphic_value<TypedScalarValueInputView<T>>;

template <typename T> class BorrowedScalarValue final : public TypedScalarValueInputView<T> {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = scalar_value_t<T>;

  public:
    [[nodiscard]] Value value() const noexcept override {
        return Traits::read(*reinterpret_cast<const Stored *>(this->value_));
    }

    void release() noexcept override {}
};

template <typename T> class BorrowedScalarSource final : public TypedScalarValueInputView<T> {
    using Value = scalar_value_t<T>;

  public:
    [[nodiscard]] Value value() const noexcept override {
        return *reinterpret_cast<const std::remove_reference_t<Value> *>(this->value_);
    }

    void release() noexcept override {}
};

template <typename T>
[[nodiscard]] TypedScalarView<T> make_borrowed_scalar_source(const scalar_value_t<T> &value) noexcept {
    auto result = TypedScalarView<T>::template make<BorrowedScalarSource<T>>();
    result->bind(std::addressof(value));

    return result;
}

template <typename T> class BorrowedMutableScalarList final : public TypedScalarListMutableInputView<T> {
    using Traits = scalar_storage_traits<List<T>>;
    using Stored = typename Traits::stored_type;

  public:
    [[nodiscard]] List<T> value() const noexcept override {
        return Traits::read(*reinterpret_cast<const Stored *>(this->value_));
    }

    void release() noexcept override {}
};

template <typename T> class BorrowedMutableScalarSet final : public TypedScalarSetMutableInputView<T> {
    using Traits = scalar_storage_traits<Set<T>>;
    using Stored = typename Traits::stored_type;

  public:
    [[nodiscard]] Set<T> value() const noexcept override {
        return Traits::read(*reinterpret_cast<const Stored *>(this->value_));
    }

    void release() noexcept override {}
};

template <typename K, typename V> class BorrowedMutableScalarDict final : public TypedScalarDictMutableInputView<K, V> {
    using Traits = scalar_storage_traits<Dict<K, V>>;
    using Stored = typename Traits::stored_type;

  public:
    [[nodiscard]] Dict<K, V> value() const noexcept override {
        return Traits::read(*reinterpret_cast<const Stored *>(this->value_));
    }

    void release() noexcept override {}
};

template <typename T> [[nodiscard]] scalar_value_t<T> typed_scalar_value(const ScalarValueInputView &value) {
    if (&value.type() != &cpp_scalar_type<T>()) {
        throw std::invalid_argument{"incompatible scalar types"};
    }

    return static_cast<const TypedScalarValueInputView<T> &>(value).value();
}

struct ScalarValueAllocation {
    std::pmr::memory_resource *memory;
};

enum class ScalarValueStorage {
    inline_value,
    explicit_resource,
    context_resource,
    standard_resource,
};

template <typename T>
inline constexpr bool inline_scalar_value =
    std::same_as<scalar_stored_t<T>, scalar_value_t<T>> && std::is_trivially_copyable_v<scalar_stored_t<T>> &&
    sizeof(scalar_stored_t<T>) <= sizeof(std::uintptr_t);

template <typename T, ScalarValueStorage Mode>
class ScalarValueImplementation final : public TypedScalarValueInputView<T> {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = scalar_value_t<T>;

    static constexpr auto stores_resource = Mode == ScalarValueStorage::explicit_resource;
    static constexpr auto header_size = stores_resource ? sizeof(ScalarValueAllocation) : std::size_t{};
    static constexpr auto header_alignment = stores_resource ? alignof(ScalarValueAllocation) : std::size_t{1};
    static constexpr auto alignment = std::max(header_alignment, alignof(Stored));
    static constexpr auto value_offset = (header_size + alignof(Stored) - 1) / alignof(Stored) * alignof(Stored);
    static constexpr auto allocation_size = value_offset + sizeof(Stored);

  public:
    template <typename Source>
    [[nodiscard]] static std::uintptr_t make(Source &&value, std::pmr::memory_resource &memory) {
        if constexpr (Mode == ScalarValueStorage::inline_value) {
            auto result = std::uintptr_t{};
            std::memcpy(&result, &value, sizeof(value));

            return result;
        }

        auto *allocation = static_cast<std::byte *>(memory.allocate(allocation_size, alignment));
        if constexpr (stores_resource) {
            std::construct_at(reinterpret_cast<ScalarValueAllocation *>(allocation), &memory);
        }

        auto *stored = reinterpret_cast<Stored *>(allocation + value_offset);
        auto release = util::scope_exit{[&]() noexcept { memory.deallocate(allocation, allocation_size, alignment); }};
        Traits::construct(stored, std::forward<Source>(value), memory);
        release.release();

        if constexpr (Mode == ScalarValueStorage::context_resource) {
#ifndef NDEBUG
            record_context_scalar_allocation(stored);
#endif
        }

        return reinterpret_cast<std::uintptr_t>(stored);
    }

    [[nodiscard]] Value value() const noexcept override {
        if constexpr (Mode == ScalarValueStorage::inline_value) {
            auto bytes = std::array<std::byte, sizeof(Value)>{};
            std::memcpy(bytes.data(), &this->value_, bytes.size());

            return std::bit_cast<Value>(bytes);
        }

        return Traits::read(*reinterpret_cast<const Stored *>(this->value_));
    }

    void release() noexcept override {
        if constexpr (Mode == ScalarValueStorage::inline_value) {
            return;
        }

        if (this->value_ == 0) {
            return;
        }

        auto *stored = reinterpret_cast<Stored *>(this->value_);
        auto *allocation = reinterpret_cast<std::byte *>(stored) - value_offset;
        auto *memory = resource(allocation, stored);

        Traits::destroy(stored, *memory);
        memory->deallocate(allocation, allocation_size, alignment);
        this->value_ = 0;
    }

  private:
    [[nodiscard]] static std::pmr::memory_resource *resource(std::byte *allocation, const Stored *stored) noexcept {
        if constexpr (Mode == ScalarValueStorage::explicit_resource) {
            return reinterpret_cast<ScalarValueAllocation *>(allocation)->memory;
        } else if constexpr (Mode == ScalarValueStorage::context_resource) {
#ifndef NDEBUG
            check_context_scalar_deallocation(stored);
#endif
            const auto *owner = current_scalar_allocation_owner();
            assert(owner != nullptr);

            return owner->memory;
        } else {
            return std::pmr::new_delete_resource();
        }
    }
};

template <typename Interface, typename Implementation, typename Source>
[[nodiscard]] util::polymorphic_value<Interface> make_scalar_value(Source &&value, std::pmr::memory_resource &memory) {
    auto result = util::polymorphic_value<Interface>::template make<Implementation>();
    result->bind(Implementation::make(std::forward<Source>(value), memory));

    return result;
}

template <typename Interface, typename T, typename Source>
[[nodiscard]] util::polymorphic_value<Interface> make_scalar_value(Source &&value, std::pmr::memory_resource *memory) {
    using Storage = ScalarValueStorage;

    if constexpr (inline_scalar_value<T>) {
        using Implementation = ScalarValueImplementation<T, Storage::inline_value>;
        return make_scalar_value<Interface, Implementation>(std::forward<Source>(value),
                                                            *std::pmr::null_memory_resource());
    }

    if (memory != nullptr) {
        using Implementation = ScalarValueImplementation<T, Storage::explicit_resource>;
        return make_scalar_value<Interface, Implementation>(std::forward<Source>(value), *memory);
    }

    if (const auto *owner = current_scalar_allocation_owner()) {
        using Implementation = ScalarValueImplementation<T, Storage::context_resource>;
        return make_scalar_value<Interface, Implementation>(std::forward<Source>(value), *owner->memory);
    }

    using Implementation = ScalarValueImplementation<T, Storage::standard_resource>;
    return make_scalar_value<Interface, Implementation>(std::forward<Source>(value), *std::pmr::new_delete_resource());
}

template <typename Interface, typename T>
[[nodiscard]] util::polymorphic_value<Interface> make_borrowed_scalar(const scalar_stored_t<T> *value) {
    auto result = util::polymorphic_value<Interface>::template make<BorrowedScalarValue<T>>();
    result->bind(value);

    return result;
}

} // namespace details

namespace details {

template <typename T>
inline constexpr bool scalar_type_supports_hash = [] {
    if constexpr (scalar_variable<T>) {
        return scalar_variable_supports_hash<T>;
    } else if constexpr (is_list_v<T> || is_set_v<T>) {
        return scalar_type_supports_hash<typename T::item_type>;
    } else if constexpr (is_dict_v<T>) {
        return scalar_type_supports_hash<typename T::key_type> && scalar_type_supports_hash<typename T::mapped_type>;
    } else {
        return scalar_storage_traits<T>::supports_hash;
    }
}();

template <typename T>
inline constexpr bool scalar_type_supports_equality = [] {
    if constexpr (scalar_variable<T>) {
        return scalar_variable_supports_equality<T>;
    } else if constexpr (is_list_v<T> || is_set_v<T>) {
        return scalar_type_supports_equality<typename T::item_type>;
    } else if constexpr (is_dict_v<T>) {
        return scalar_type_supports_equality<typename T::key_type> &&
               scalar_type_supports_equality<typename T::mapped_type>;
    } else {
        return scalar_storage_traits<T>::supports_equality;
    }
}();

} // namespace details

template <typename T> class Scalar {
    static constexpr auto variable = details::scalar_variable<T>;
    static constexpr auto erased = std::same_as<T, void> || variable;
    using Interface = std::conditional_t<erased, ScalarValueInputView, TypedScalarValueInputView<T>>;
    using View = util::polymorphic_value<Interface>;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    [[nodiscard]] const Interface *operator->() const noexcept
        requires(!variable)
    {
        return value_.operator->();
    }

    [[nodiscard]] const Scalar *operator->() const noexcept
        requires variable
    {
        return this;
    }

    [[nodiscard]] details::scalar_value_t<T> value() const noexcept
        requires(!erased)
    {
        return value_->value();
    }

    [[nodiscard]] const ScalarType &type() const noexcept { return value_->type(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_variable_supports_hash<T>
    {
        return value_->erased_hash();
    }

    [[nodiscard]] bool equals(const ScalarInputView &other) const noexcept
        requires details::scalar_variable_supports_equality<T>
    {
        return value_->erased_equals(static_cast<const ScalarValueInputView &>(other));
    }

    [[nodiscard]] const Interface &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const Interface &input_view() const && = delete;

    using scalar_type = T;

    template <typename Value = T>
        requires(!erased && std::same_as<Value, T>)
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Value> &value) noexcept {
        return Scalar{details::make_borrowed_scalar<Interface, Value>(&value)};
    }

    template <typename Value = T>
        requires(!erased && std::same_as<Value, T>)
    static Scalar from(details::scalar_stored_t<Value> &&) = delete;

    template <typename Value>
        requires erased
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Value> &value) noexcept {
        return Scalar{details::make_borrowed_scalar<Interface, Value>(&value)};
    }

    template <typename Value>
        requires erased
    static Scalar from(details::scalar_stored_t<Value> &&) = delete;

    [[nodiscard]] static Scalar from(View value) noexcept
        requires erased
    {
        return Scalar{value};
    }

    [[nodiscard]] static Scalar from(const ScalarValueInputView &value) noexcept
        requires erased
    {
        return Scalar{View::copy(value)};
    }

    [[nodiscard]] static Scalar from(const Interface &value) noexcept
        requires(!erased)
    {
        return Scalar{View::copy(value)};
    }

    [[nodiscard]] static Scalar from(const ScalarValueInputView &prototype, const void *data) noexcept {
        auto result = [&] {
            if constexpr (erased) {
                return View::copy(prototype);
            } else {
                return View::copy(static_cast<const Interface &>(prototype));
            }
        }();
        result->bind(data);

        return Scalar{result};
    }

  private:
    explicit Scalar(View value) noexcept : value_(value) {}

    View value_;
};

template <typename T> class Scalar<List<T>> {
    using View = util::polymorphic_value<ScalarListInputView>;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    Scalar(const ScalarListInputView &prototype, const void *value) noexcept : value_(View::copy(prototype)) {
        value_->bind(value);
    }

    [[nodiscard]] std::size_t size() const noexcept { return value_->size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] Scalar<T> operator[](std::size_t index) const {
        const auto [prototype, data] = value_.get()[index];
        return Scalar<T>::from(prototype, data);
    }

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }
    [[nodiscard]] const ScalarType &type() const noexcept { return value_->type(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_type_supports_hash<List<T>>
    {
        return value_->erased_hash();
    }

    [[nodiscard]] bool equals(const ScalarInputView &other) const noexcept
        requires details::scalar_type_supports_equality<List<T>>
    {
        return value_->erased_equals(static_cast<const ScalarValueInputView &>(other));
    }

    [[nodiscard]] const ScalarListInputView &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const ScalarListInputView &input_view() const && = delete;

    [[nodiscard]] static Scalar from(const ScalarValueInputView &prototype, const void *data) noexcept {
        auto result = View::copy(static_cast<const ScalarListInputView &>(prototype));
        result->bind(data);

        return Scalar<List<T>>{result};
    }

    [[nodiscard]] static Scalar from(const ScalarListInputView &value) noexcept { return Scalar{View::copy(value)}; }

    [[nodiscard]] static Scalar from(const details::scalar_stored_t<List<T>> &value) noexcept {
        auto result = View::template make<details::BorrowedScalarValue<List<T>>>();
        result->bind(std::addressof(value));

        return Scalar<List<T>>{result};
    }

    static Scalar from(details::scalar_stored_t<List<T>> &&) = delete;

    using scalar_type = List<T>;

  private:
    explicit Scalar(View value) noexcept : value_{value} {}

    View value_;
};

template <typename T> class Scalar<Set<T>> {
    using View = util::polymorphic_value<ScalarSetInputView>;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    Scalar(const ScalarSetInputView &prototype, const void *value) noexcept : value_(View::copy(prototype)) {
        value_->bind(value);
    }

    [[nodiscard]] std::size_t size() const noexcept { return value_->size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool contains(const Scalar<T> &item) const { return value_->contains(item.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<T> && std::same_as<canonical_scalar_t<Source>, T>)
    [[nodiscard]] bool contains(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        const auto view = details::make_borrowed_scalar_source<T>(value);
        return value_->contains(view.get());
    }

    template <typename Function> void for_each(Function &&function) const {
        value_->for_each(std::addressof(function), [](void *context, ScalarSetInputView::Item item) {
            auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
            std::invoke(callback, Scalar<T>::from(item.prototype, item.data));
        });
    }

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }
    [[nodiscard]] const ScalarType &type() const noexcept { return value_->type(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_type_supports_hash<Set<T>>
    {
        return value_->erased_hash();
    }

    [[nodiscard]] bool equals(const ScalarInputView &other) const noexcept
        requires details::scalar_type_supports_equality<Set<T>>
    {
        return value_->erased_equals(static_cast<const ScalarValueInputView &>(other));
    }

    [[nodiscard]] const ScalarSetInputView &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const ScalarSetInputView &input_view() const && = delete;

    [[nodiscard]] static Scalar from(const ScalarValueInputView &prototype, const void *data) noexcept {
        auto result = View::copy(static_cast<const ScalarSetInputView &>(prototype));
        result->bind(data);

        return Scalar<Set<T>>{result};
    }

    [[nodiscard]] static Scalar from(const ScalarSetInputView &value) noexcept { return Scalar{View::copy(value)}; }

    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Set<T>> &value) noexcept {
        auto result = View::template make<details::BorrowedScalarValue<Set<T>>>();
        result->bind(std::addressof(value));

        return Scalar<Set<T>>{result};
    }

    static Scalar from(details::scalar_stored_t<Set<T>> &&) = delete;

    using scalar_type = Set<T>;

  private:
    explicit Scalar(View value) noexcept : value_{value} {}

    View value_;
};

template <typename K, typename V> class Scalar<Dict<K, V>> {
    using View = util::polymorphic_value<ScalarDictInputView>;

  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    Scalar(const ScalarDictInputView &prototype, const void *value) noexcept : value_(View::copy(prototype)) {
        value_->bind(value);
    }

    [[nodiscard]] std::size_t size() const noexcept { return value_->size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool contains(const Scalar<K> &key) const { return value_->contains(key.input_view()); }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] bool contains(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        return value_->contains(view.get());
    }

    [[nodiscard]] Scalar<V> at(const Scalar<K> &key) const {
        const auto [prototype, data] = value_->at(key.input_view());
        return Scalar<V>::from(prototype, data);
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<K> && std::same_as<canonical_scalar_t<Source>, K>)
    [[nodiscard]] Scalar<V> at(Source &&key) const {
        const auto value = canonical_scalar(std::forward<Source>(key));
        const auto view = details::make_borrowed_scalar_source<K>(value);
        const auto [prototype, data] = value_->at(view.get());
        return Scalar<V>::from(prototype, data);
    }

    template <typename Function> void for_each(Function &&function) const {
        value_->for_each(std::addressof(function), [](void *context, ScalarDictInputView::Entry entry) {
            auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
            std::invoke(callback, Scalar<K>::from(entry.key.prototype, entry.key.data),
                        Scalar<V>::from(entry.value.prototype, entry.value.data));
        });
    }

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }
    [[nodiscard]] const ScalarType &type() const noexcept { return value_->type(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_type_supports_hash<Dict<K, V>>
    {
        return value_->erased_hash();
    }

    [[nodiscard]] bool equals(const ScalarInputView &other) const noexcept
        requires details::scalar_type_supports_equality<Dict<K, V>>
    {
        return value_->erased_equals(static_cast<const ScalarValueInputView &>(other));
    }

    [[nodiscard]] const ScalarDictInputView &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const ScalarDictInputView &input_view() const && = delete;

    [[nodiscard]] static Scalar from(const ScalarValueInputView &prototype, const void *data) noexcept {
        auto result = View::copy(static_cast<const ScalarDictInputView &>(prototype));
        result->bind(data);

        return Scalar<Dict<K, V>>{result};
    }

    [[nodiscard]] static Scalar from(const ScalarDictInputView &value) noexcept { return Scalar{View::copy(value)}; }

    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Dict<K, V>> &value) noexcept {
        auto result = View::template make<details::BorrowedScalarValue<Dict<K, V>>>();
        result->bind(std::addressof(value));

        return Scalar<Dict<K, V>>{result};
    }

    static Scalar from(details::scalar_stored_t<Dict<K, V>> &&) = delete;

    using scalar_type = Dict<K, V>;

  private:
    explicit Scalar(View value) noexcept : value_{value} {}

    View value_;
};

template <typename T> class ScalarValue {
    using Interface = TypedScalarValueInputView<T>;
    using View = util::polymorphic_value<Interface>;

  public:
    template <scalar_source Source>
        requires std::same_as<canonical_scalar_t<Source>, T>
    ScalarValue(Source &&source)
        : value_(details::make_scalar_value<Interface, T>(canonical_scalar(std::forward<Source>(source)), nullptr)) {}

    template <typename Source>
        requires details::scalar_type_traits<T>::is_opaque &&
                 std::same_as<std::remove_cvref_t<Source>, typename T::value_type>
    explicit ScalarValue(Source &&source)
        : value_(details::make_scalar_value<Interface, T>(std::forward<Source>(source), nullptr)) {}

    template <scalar_source Source>
        requires std::same_as<canonical_scalar_t<Source>, T>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(canonical_scalar(std::forward<Source>(source)), &memory)) {}

    template <typename Source>
        requires details::scalar_type_traits<T>::is_opaque &&
                 std::same_as<std::remove_cvref_t<Source>, typename T::value_type>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(std::forward<Source>(source), &memory)) {}

    explicit ScalarValue(const Scalar<T> &source)
        : value_(details::make_scalar_value<Interface, T>(
              static_cast<const TypedScalarValueInputView<T> &>(source.input_view()).value(), nullptr)) {}

    ScalarValue(const Scalar<T> &source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(
              static_cast<const TypedScalarValueInputView<T> &>(source.input_view()).value(), &memory)) {}

    template <std::ranges::contiguous_range Range>
        requires is_list_v<T> &&
                 (std::convertible_to<std::ranges::range_reference_t<const Range>,
                                      details::scalar_value_t<typename T::item_type>> ||
                  details::scalar_view_source_for<std::ranges::range_reference_t<const Range>, typename T::item_type>)
    explicit ScalarValue(const Range &source) : value_(details::make_scalar_value<Interface, T>(source, nullptr)) {}

    template <std::ranges::contiguous_range Range>
        requires is_list_v<T> &&
                 (std::convertible_to<std::ranges::range_reference_t<const Range>,
                                      details::scalar_value_t<typename T::item_type>> ||
                  details::scalar_view_source_for<std::ranges::range_reference_t<const Range>, typename T::item_type>)
    ScalarValue(const Range &source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(source, &memory)) {}

    template <std::ranges::input_range Range>
        requires is_set_v<T> && std::convertible_to<std::ranges::range_reference_t<const Range>,
                                                    details::scalar_value_t<typename T::item_type>>
    explicit ScalarValue(const Range &source) : value_(details::make_scalar_value<Interface, T>(source, nullptr)) {}

    template <std::ranges::input_range Range>
        requires is_set_v<T> && std::convertible_to<std::ranges::range_reference_t<const Range>,
                                                    details::scalar_value_t<typename T::item_type>>
    ScalarValue(const Range &source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(source, &memory)) {}

    template <std::ranges::input_range Range>
        requires is_dict_v<T> &&
                 requires(std::ranges::range_reference_t<const Range> entry) {
                     { entry.first } -> std::convertible_to<details::scalar_value_t<typename T::key_type>>;
                 } &&
                 (std::convertible_to<decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                                      details::scalar_value_t<typename T::mapped_type>> ||
                  details::scalar_view_source_for<
                      decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                      typename T::mapped_type>)
    explicit ScalarValue(const Range &source) : value_(details::make_scalar_value<Interface, T>(source, nullptr)) {}

    template <std::ranges::input_range Range>
        requires is_dict_v<T> &&
                 requires(std::ranges::range_reference_t<const Range> entry) {
                     { entry.first } -> std::convertible_to<details::scalar_value_t<typename T::key_type>>;
                 } &&
                 (std::convertible_to<decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                                      details::scalar_value_t<typename T::mapped_type>> ||
                  details::scalar_view_source_for<
                      decltype(std::declval<std::ranges::range_reference_t<const Range>>().second),
                      typename T::mapped_type>)
    ScalarValue(const Range &source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(source, &memory)) {}

    ~ScalarValue() { value_->release(); }

    ScalarValue(const ScalarValue &) = delete;
    ScalarValue &operator=(const ScalarValue &) = delete;

    ScalarValue(ScalarValue &&other) noexcept : value_(other.value_) { other.value_->bind(std::uintptr_t{}); }

    ScalarValue &operator=(ScalarValue &&other) noexcept {
        value_->release();
        value_ = other.value_;
        other.value_->bind(std::uintptr_t{});

        return *this;
    }

    [[nodiscard]] Scalar<T> view() const & noexcept { return Scalar<T>::from(value_.get()); }
    [[nodiscard]] Scalar<T> view() const && = delete;
    [[nodiscard]] const Interface &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const Interface &input_view() const && = delete;

    using scalar_type = T;

  private:
    View value_;
};

template <> class ScalarValue<> {
    using View = details::ErasedScalarView;

  public:
    template <typename T> explicit ScalarValue(const Scalar<T> &source) : value_(copy(source, nullptr)) {}

    template <typename T>
    ScalarValue(const Scalar<T> &source, std::pmr::memory_resource &memory) : value_(copy(source, &memory)) {}

    template <typename T> explicit ScalarValue(const ScalarValue<T> &source) : value_(copy(source, nullptr)) {}

    template <typename T>
    ScalarValue(const ScalarValue<T> &source, std::pmr::memory_resource &memory) : value_(copy(source, &memory)) {}

    ~ScalarValue() { value_->release(); }

    ScalarValue(const ScalarValue &) = delete;

    ScalarValue &operator=(const ScalarValue &source) {
        if (this != &source) {
            auto replacement = ScalarValue{source.input_view()};
            *this = std::move(replacement);
        }

        return *this;
    }

    template <typename T> ScalarValue &operator=(const Scalar<T> &source) {
        auto replacement = ScalarValue{source};
        *this = std::move(replacement);

        return *this;
    }

    template <typename T> ScalarValue &operator=(const ScalarValue<T> &source) {
        auto replacement = ScalarValue{source};
        *this = std::move(replacement);

        return *this;
    }

    ScalarValue(ScalarValue &&other) noexcept : value_(other.value_) { other.value_->bind(std::uintptr_t{}); }

    ScalarValue &operator=(ScalarValue &&other) noexcept {
        value_->release();
        value_ = other.value_;
        other.value_->bind(std::uintptr_t{});

        return *this;
    }

    [[nodiscard]] Scalar<> view() const & noexcept { return Scalar<>::from(value_.get()); }
    [[nodiscard]] Scalar<> view() const && = delete;
    [[nodiscard]] const ScalarValueInputView &input_view() const & noexcept { return value_.get(); }
    [[nodiscard]] const ScalarValueInputView &input_view() const && = delete;

    using scalar_type = void;

  private:
    explicit ScalarValue(const ScalarValueInputView &source) : value_(copy(source, nullptr)) {}

    template <typename T> [[nodiscard]] static View copy(const Scalar<T> &source, std::pmr::memory_resource *memory) {
        return copy(source.input_view(), memory);
    }

    template <typename T>
    [[nodiscard]] static View copy(const ScalarValue<T> &source, std::pmr::memory_resource *memory) {
        return copy(source.input_view(), memory);
    }

    [[nodiscard]] static View copy(const ScalarValueInputView &source, std::pmr::memory_resource *memory) {
        auto result = details::make_borrowed_scalar<ScalarValueInputView, std::int64_t>(nullptr);
        source.copy_value(&result, memory);

        return result;
    }

    View value_;
};

template <typename T, typename Base>
void TypedScalarValueInputViewBase<T, Base>::copy_value(void *destination, std::pmr::memory_resource *memory) const {
    auto copy = details::make_scalar_value<ScalarValueInputView, T>(value(), memory);
    *static_cast<details::ErasedScalarView *>(destination) = copy;
}

template <typename T>
ScalarListInputView::Item TypedScalarValueInputView<List<T>>::operator[](std::size_t index) const noexcept {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<T>>();

    return {prototype.get(), std::addressof(storage().items[static_cast<std::uint32_t>(index)])};
}

template <typename T> bool TypedScalarValueInputView<Set<T>>::contains(const ScalarValueInputView &value) const {
    return storage().items.contains(details::typed_scalar_value<T>(value));
}

template <typename T>
void TypedScalarValueInputView<Set<T>>::for_each(void *context, ScalarSetInputView::ItemFunction function) const {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<T>>();

    for (const auto &item : storage().items) {
        function(context, {prototype.get(), std::addressof(item)});
    }
}

template <typename K, typename V>
bool TypedScalarValueInputView<Dict<K, V>>::contains(const ScalarValueInputView &key) const {
    return storage().items.contains(details::typed_scalar_value<K>(key));
}

template <typename K, typename V>
ScalarDictInputView::Item TypedScalarValueInputView<Dict<K, V>>::at(const ScalarValueInputView &key) const {
    const auto &stored = storage().items.at(details::typed_scalar_value<K>(key));
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<V>>();

    return {prototype.get(), std::addressof(stored)};
}

template <typename K, typename V>
void TypedScalarValueInputView<Dict<K, V>>::for_each(void *context, ScalarDictInputView::EntryFunction function) const {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto key_prototype = View::template make<details::BorrowedScalarValue<K>>();
    static const auto value_prototype = View::template make<details::BorrowedScalarValue<V>>();

    for (const auto [key, value] : storage().items) {
        function(context, {{key_prototype.get(), std::addressof(key)}, {value_prototype.get(), std::addressof(value)}});
    }
}

template <typename T>
ScalarListInputView::Item TypedScalarListMutableInputView<T>::operator[](std::size_t index) const noexcept {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<T>>();

    return {prototype.get(), std::addressof(storage().items[static_cast<std::uint32_t>(index)])};
}

template <typename T> void TypedScalarListMutableInputView<T>::append(const ScalarValueInputView &value) const {
    auto &memory = details::current_scalar_memory_resource();
    storage().items.emplace_back_with(
        memory, [&](auto *item) { value.copy_construct(item, cpp_scalar_storage_representation<T>(), memory); });
}

template <typename T>
void TypedScalarListMutableInputView<T>::set(std::size_t index, const ScalarValueInputView &value) const {
    value.assign(std::addressof(storage().items[static_cast<std::uint32_t>(index)]),
                 cpp_scalar_storage_representation<T>());
}

template <typename T> void TypedScalarListMutableInputView<T>::erase(std::size_t index) const {
    auto &memory = details::current_scalar_memory_resource();
    auto &items = storage().items;

    for (auto i = static_cast<std::uint32_t>(index); i + 1 < items.size(); ++i) {
        items[i] = std::move(items[i + 1]);
    }
    items.pop_back_with(memory, [&](auto *item) noexcept { details::scalar_storage_traits<T>::destroy(item, memory); });
}

template <typename T> void TypedScalarListMutableInputView<T>::clear() const {
    auto &memory = details::current_scalar_memory_resource();
    storage().items.release_with(
        memory, [&](auto *item) noexcept { details::scalar_storage_traits<T>::destroy(item, memory); });
}

template <typename T> bool TypedScalarSetMutableInputView<T>::contains(const ScalarValueInputView &value) const {
    return storage().items.contains(details::typed_scalar_value<T>(value));
}

template <typename T>
void TypedScalarSetMutableInputView<T>::for_each(void *context, ScalarSetInputView::ItemFunction function) const {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<T>>();

    for (const auto &item : storage().items) {
        function(context, {prototype.get(), std::addressof(item)});
    }
}

template <typename T> bool TypedScalarSetMutableInputView<T>::insert(const ScalarValueInputView &value) const {
    const auto item = details::typed_scalar_value<T>(value);
    auto &memory = details::current_scalar_memory_resource();

    return storage()
        .items
        .insert_with(
            item, memory, [&](auto *stored) { details::scalar_storage_traits<T>::construct(stored, item, memory); },
            [&](auto *stored) noexcept { details::scalar_storage_traits<T>::destroy(stored, memory); })
        .second;
}

template <typename T> bool TypedScalarSetMutableInputView<T>::erase(const ScalarValueInputView &value) const {
    auto &memory = details::current_scalar_memory_resource();
    return storage().items.erase_with(details::typed_scalar_value<T>(value), memory, [&](auto *stored) noexcept {
        details::scalar_storage_traits<T>::destroy(stored, memory);
    });
}

template <typename T> void TypedScalarSetMutableInputView<T>::clear() const {
    auto &memory = details::current_scalar_memory_resource();
    storage().items.release_with(
        memory, [&](auto *stored) noexcept { details::scalar_storage_traits<T>::destroy(stored, memory); });
}

template <typename K, typename V>
bool TypedScalarDictMutableInputView<K, V>::contains(const ScalarValueInputView &key) const {
    return storage().items.contains(details::typed_scalar_value<K>(key));
}

template <typename K, typename V>
ScalarDictInputView::Item TypedScalarDictMutableInputView<K, V>::at(const ScalarValueInputView &key) const {
    const auto &stored = storage().items.at(details::typed_scalar_value<K>(key));
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto prototype = View::template make<details::BorrowedScalarValue<V>>();

    return {prototype.get(), std::addressof(stored)};
}

template <typename K, typename V>
void TypedScalarDictMutableInputView<K, V>::for_each(void *context, ScalarDictInputView::EntryFunction function) const {
    using View = util::polymorphic_value<ScalarValueInputView>;
    static const auto key_prototype = View::template make<details::BorrowedScalarValue<K>>();
    static const auto value_prototype = View::template make<details::BorrowedScalarValue<V>>();

    for (const auto [key, value] : storage().items) {
        function(context, {{key_prototype.get(), std::addressof(key)}, {value_prototype.get(), std::addressof(value)}});
    }
}

template <typename K, typename V>
void TypedScalarDictMutableInputView<K, V>::insert_or_assign(const ScalarValueInputView &key,
                                                             const ScalarValueInputView &value) const {
    const auto key_value = details::typed_scalar_value<K>(key);
    const auto mapped_value = details::typed_scalar_value<V>(value);
    auto &memory = details::current_scalar_memory_resource();
    auto [stored, inserted] = storage().items.try_emplace_with(
        key_value, memory,
        [&](auto *destination) { details::scalar_storage_traits<K>::construct(destination, key_value, memory); },
        [&](auto *destination) noexcept { details::scalar_storage_traits<K>::destroy(destination, memory); },
        [&](auto *destination) { details::scalar_storage_traits<V>::construct(destination, mapped_value, memory); });
    if (!inserted) {
        if constexpr (requires { details::scalar_storage_traits<V>::assign(*stored, mapped_value, memory); }) {
            details::scalar_storage_traits<V>::assign(*stored, mapped_value, memory);
        } else {
            details::scalar_storage_traits<V>::assign(*stored, mapped_value);
        }
    }
}

template <typename K, typename V>
bool TypedScalarDictMutableInputView<K, V>::erase(const ScalarValueInputView &key) const {
    auto &memory = details::current_scalar_memory_resource();
    return storage().items.erase_with(
        details::typed_scalar_value<K>(key), memory,
        [&](auto *stored) noexcept { details::scalar_storage_traits<K>::destroy(stored, memory); },
        [&](auto *stored) noexcept { details::scalar_storage_traits<V>::destroy(stored, memory); });
}

template <typename K, typename V> void TypedScalarDictMutableInputView<K, V>::clear() const {
    auto &memory = details::current_scalar_memory_resource();
    storage().items.release_with(
        memory, [&](auto *stored) noexcept { details::scalar_storage_traits<K>::destroy(stored, memory); },
        [&](auto *stored) noexcept { details::scalar_storage_traits<V>::destroy(stored, memory); });
}

template <scalar_source Source> ScalarValue(Source &&) -> ScalarValue<canonical_scalar_t<Source>>;

template <scalar_source Source>
ScalarValue(Source &&, std::pmr::memory_resource &) -> ScalarValue<canonical_scalar_t<Source>>;

namespace details {

template <typename T> struct scalar_handle : std::false_type {};
template <typename T> struct scalar_handle<Scalar<T>> : std::true_type {};
template <typename T> struct scalar_handle<ScalarValue<T>> : std::true_type {};

template <typename T>
concept ScalarHandle = scalar_handle<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool scalar_handle_supports_equality = [] {
    using ScalarType = typename std::remove_cvref_t<T>::scalar_type;

    if constexpr (std::same_as<ScalarType, void>) {
        return false;
    } else {
        return scalar_type_supports_equality<ScalarType>;
    }
}();

template <typename T>
concept EqualityComparableScalarHandle = ScalarHandle<T> && scalar_handle_supports_equality<T>;

} // namespace details

template <details::EqualityComparableScalarHandle Left, details::EqualityComparableScalarHandle Right>
[[nodiscard]] bool operator==(const Left &left, const Right &right) noexcept {
    using LeftScalar = typename std::remove_cvref_t<Left>::scalar_type;
    using RightScalar = typename std::remove_cvref_t<Right>::scalar_type;

    if constexpr (details::scalar_variable<LeftScalar> || details::scalar_variable_list<LeftScalar>) {
        return left.equals(right.input_view());
    } else if constexpr (details::scalar_variable<RightScalar> || details::scalar_variable_list<RightScalar>) {
        return right.equals(left.input_view());
    } else if constexpr (std::same_as<LeftScalar, RightScalar>) {
        return left.input_view().equals(right.input_view());
    } else {
        return false;
    }
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const Scalar<Source> &source) noexcept {
    const auto &typed = static_cast<const TypedScalarValueInputView<Target> &>(source.input_view());

    return Scalar<Target>::from(typed);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const Scalar<Source> &source) {
    auto bindings = TypeMap{};
    if (!cpp_scalar_type<Target>().match(source.input_view().type(), bindings)) {
        throw std::bad_cast{};
    }

    return unchecked_scalar_cast<Target>(source);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const ScalarValue<Source> &source) {
    return scalar_cast<Target>(source.view());
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const ScalarValue<Source> &source) noexcept {
    return unchecked_scalar_cast<Target>(source.view());
}

static_assert(sizeof(Scalar<>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<SCALAR>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Set<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Dict<SCALAR, SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<EvaluationTime>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<string>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<>) == 2 * sizeof(void *));

} // namespace bg
