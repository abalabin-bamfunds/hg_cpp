#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/type.hpp>
#include <bg/scalar/view.hpp>
#include <bg/type/type_map.hpp>
#include <bg/util/polymorphic_value.hpp>

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
        try {
            Traits::construct(stored, std::forward<Source>(value), memory);
        } catch (...) {
            memory.deallocate(allocation, allocation_size, alignment);
            throw;
        }

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

        std::destroy_at(stored);
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

template <typename T> class Scalar {
    static constexpr auto variable = details::scalar_variable<T>;
    static constexpr auto erased = std::same_as<T, void> || variable;
    using Interface = std::conditional_t<erased, ScalarValueInputView, TypedScalarValueInputView<T>>;
    using View = util::polymorphic_value<Interface>;

  public:
    Scalar(const Scalar &) = delete;
    Scalar &operator=(const Scalar &) = delete;
    Scalar(Scalar &&) = delete;
    Scalar &operator=(Scalar &&) = delete;

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

    [[nodiscard]] const ScalarType &type() const noexcept
        requires variable
    {
        return value_->type();
    }

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

    [[nodiscard]] const Interface &view() const noexcept { return value_.get(); }

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

  private:
    explicit Scalar(View value) noexcept : value_(value) {}

    View value_;
};

template <details::scalar_variable Variable> class Scalar<List<Variable>> {
    using View = util::polymorphic_value<ScalarListInputView>;

  public:
    Scalar(const Scalar &) = delete;
    Scalar &operator=(const Scalar &) = delete;
    Scalar(Scalar &&) = delete;
    Scalar &operator=(Scalar &&) = delete;

    Scalar(const ScalarListInputView &prototype, const void *value) noexcept : value_(View::copy(prototype)) {
        value_->bind(value);
    }

    [[nodiscard]] std::size_t size() const noexcept { return value_->size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] Scalar<Variable> operator[](std::size_t index) const {
        const auto [prototype, data] = value_.get()[index];
        auto item = details::ErasedScalarView::copy(prototype);
        item->bind(data);

        return Scalar<Variable>::from(item);
    }

    [[nodiscard]] const Scalar *operator->() const noexcept { return this; }
    [[nodiscard]] const ScalarType &type() const noexcept { return value_->type(); }

    [[nodiscard]] std::size_t hash() const noexcept
        requires details::scalar_variable_supports_hash<Variable>
    {
        return value_->erased_hash();
    }

    [[nodiscard]] bool equals(const ScalarInputView &other) const noexcept
        requires details::scalar_variable_supports_equality<Variable>
    {
        return value_->erased_equals(static_cast<const ScalarValueInputView &>(other));
    }

    [[nodiscard]] const ScalarListInputView &view() const noexcept { return value_.get(); }

    using scalar_type = List<Variable>;

  private:
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
        requires is_opaque_v<T> && std::same_as<std::remove_cvref_t<Source>, typename T::value_type>
    explicit ScalarValue(Source &&source)
        : value_(details::make_scalar_value<Interface, T>(std::forward<Source>(source), nullptr)) {}

    template <scalar_source Source>
        requires std::same_as<canonical_scalar_t<Source>, T>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(canonical_scalar(std::forward<Source>(source)), &memory)) {}

    template <typename Source>
        requires is_opaque_v<T> && std::same_as<std::remove_cvref_t<Source>, typename T::value_type>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(std::forward<Source>(source), &memory)) {}

    explicit ScalarValue(const Scalar<T> &source)
        : value_(details::make_scalar_value<Interface, T>(source->value(), nullptr)) {}

    ScalarValue(const Scalar<T> &source, std::pmr::memory_resource &memory)
        : value_(details::make_scalar_value<Interface, T>(source->value(), &memory)) {}

    template <std::ranges::contiguous_range Range>
        requires is_list_v<T> && std::convertible_to<std::ranges::range_reference_t<const Range>,
                                                     details::scalar_value_t<typename T::item_type>>
    explicit ScalarValue(const Range &source) : value_(details::make_scalar_value<Interface, T>(source, nullptr)) {}

    template <std::ranges::contiguous_range Range>
        requires is_list_v<T> && std::convertible_to<std::ranges::range_reference_t<const Range>,
                                                     details::scalar_value_t<typename T::item_type>>
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

    [[nodiscard]] const Interface *operator->() const noexcept { return value_.operator->(); }
    [[nodiscard]] const Interface &view() const noexcept { return value_.get(); }

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
            auto replacement = ScalarValue{source.view()};
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

    [[nodiscard]] const ScalarValueInputView *operator->() const noexcept { return value_.operator->(); }
    [[nodiscard]] const ScalarValueInputView &view() const noexcept { return value_.get(); }

    using scalar_type = void;

  private:
    explicit ScalarValue(const ScalarValueInputView &source) : value_(copy(source, nullptr)) {}

    template <typename T> [[nodiscard]] static View copy(const Scalar<T> &source, std::pmr::memory_resource *memory) {
        return copy(source.view(), memory);
    }

    template <typename T>
    [[nodiscard]] static View copy(const ScalarValue<T> &source, std::pmr::memory_resource *memory) {
        return copy(source.view(), memory);
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

    return {prototype.get(), std::addressof(storage()[index])};
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

    if constexpr (scalar_variable<ScalarType>) {
        return scalar_variable_supports_equality<ScalarType>;
    } else if constexpr (scalar_variable_list<ScalarType>) {
        return scalar_variable_supports_equality<typename ScalarType::item_type>;
    } else if constexpr (std::same_as<ScalarType, void>) {
        return false;
    } else {
        return scalar_storage_traits<ScalarType>::supports_equality;
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
        return left.equals(right.view());
    } else if constexpr (details::scalar_variable<RightScalar> || details::scalar_variable_list<RightScalar>) {
        return right.equals(left.view());
    } else if constexpr (std::same_as<LeftScalar, RightScalar>) {
        return left->equals(*right.operator->());
    } else {
        return false;
    }
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const Scalar<Source> &source) noexcept {
    const auto &typed = static_cast<const TypedScalarValueInputView<Target> &>(source.view());

    return Scalar<Target>::from(typed);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const Scalar<Source> &source) {
    auto bindings = TypeMap{};
    if (!cpp_scalar_type<Target>().match(source.view().type(), bindings)) {
        throw std::bad_cast{};
    }

    return unchecked_scalar_cast<Target>(source);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const ScalarValue<Source> &source) {
    return scalar_cast<Target>(Scalar<Source>::from(source.view()));
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const ScalarValue<Source> &source) noexcept {
    return unchecked_scalar_cast<Target>(Scalar<Source>::from(source.view()));
}

static_assert(sizeof(Scalar<>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<SCALAR>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<EvaluationTime>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<string>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<>) == 2 * sizeof(void *));

} // namespace bg
