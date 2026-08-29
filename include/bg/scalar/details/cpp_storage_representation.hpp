#pragma once

#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/dict/storage_representation.hpp>
#include <bg/scalar/list/storage_representation.hpp>
#include <bg/scalar/mutable_view.hpp>
#include <bg/scalar/set/storage_representation.hpp>
#include <bg/scalar/storage_representation.hpp>
#include <bg/time_series/input_view.hpp>
#include <bg/time_series/output_view.hpp>
#include <bg/type/scalar_type.hpp>

#include <exception>
#include <memory>
#include <type_traits>

namespace bg::details {

template <typename T, typename Base> class CppScalarStorageRepresentationBase : public Base {
    using Traits = scalar_storage_traits<T>;
    using Stored = typename Traits::stored_type;
    using Value = typename Traits::value_type;

    static_assert(std::is_nothrow_destructible_v<Stored>);

  public:
    [[nodiscard]] const ScalarType &type() const noexcept override { return cpp_scalar_type<T>(); }

    [[nodiscard]] std::size_t size() const noexcept override { return sizeof(Stored); }

    [[nodiscard]] std::size_t alignment() const noexcept override { return alignof(Stored); }

    [[nodiscard]] bool uses_variable_memory() const noexcept override { return Traits::uses_variable_memory; }

    [[nodiscard]] bool requires_destruction() const noexcept override {
        return !std::is_trivially_destructible_v<Stored>;
    }

    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return std::is_trivially_copyable_v<Stored> && std::is_default_constructible_v<Stored>;
    }

    [[nodiscard]] const ScalarValueInputView *scalar_input_view_prototype() const noexcept override {
        using View = util::polymorphic_value<ScalarValueInputView>;
        static const auto prototype = View::template make<BorrowedScalarValue<T>>();

        return &prototype.get();
    }

    [[nodiscard]] const TimeSeriesValueInputView *input_view_prototype() const noexcept override {
        using View = util::polymorphic_value<TimeSeriesValueInputView>;
        static const auto prototype = View::template make<TypedTimeSeriesValueInputView<T>>();

        return &prototype.get();
    }

    [[nodiscard]] const TimeSeriesValueOutputView *output_view_prototype() const noexcept override {
        using View = util::polymorphic_value<TimeSeriesValueOutputView>;
        static const auto prototype = View::template make<TypedTimeSeriesValueOutputView<T>>();

        return &prototype.get();
    }

    void construct_initial_value(void *destination) const noexcept override {
        if constexpr (std::is_trivially_copyable_v<Stored> && std::is_default_constructible_v<Stored>) {
            std::construct_at(static_cast<Stored *>(destination), Stored{});
        } else {
            std::terminate();
        }
    }

    void construct_runtime_value(void *destination, std::pmr::memory_resource &memory) const override {
        if constexpr (std::default_initializable<Value>) {
            Traits::construct(static_cast<Stored *>(destination), Value{}, memory);
        } else {
            this->construct_initial_value(destination);
        }
    }

    void construct_from_value(void *destination, const void *value, std::pmr::memory_resource &memory) const override {
        Traits::construct(static_cast<Stored *>(destination), *static_cast<const Value *>(value), memory);
    }

    void construct_from_input(void *destination, const ScalarValueInputView &value,
                              std::pmr::memory_resource &memory) const override {
        const auto &typed = static_cast<const TypedScalarValueInputView<T> &>(value);
        Traits::construct(static_cast<Stored *>(destination), typed.value(), memory);
    }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const override {
        if constexpr (requires {
                          Traits::copy_construct(static_cast<Stored *>(destination),
                                                 *static_cast<const Stored *>(source), memory);
                      }) {
            Traits::copy_construct(static_cast<Stored *>(destination), *static_cast<const Stored *>(source), memory);
        } else {
            std::uninitialized_construct_using_allocator(static_cast<Stored *>(destination),
                                                         std::pmr::polymorphic_allocator<Stored>{&memory},
                                                         *static_cast<const Stored *>(source));
        }
    }

    void assign_from_value(void *destination, const void *value) const override {
        if constexpr (requires(Stored &target, const Value &source, std::pmr::memory_resource &memory) {
                          Traits::assign(target, source, memory);
                      }) {
            Traits::assign(*static_cast<Stored *>(destination), *static_cast<const Value *>(value),
                           current_scalar_memory_resource());
        } else if constexpr (requires(Stored &target, const Value &source) { Traits::assign(target, source); }) {
            Traits::assign(*static_cast<Stored *>(destination), *static_cast<const Value *>(value));
        } else {
            std::terminate();
        }
    }

    void assign_from_input(void *destination, const ScalarValueInputView &value) const override {
        const auto &typed = static_cast<const TypedScalarValueInputView<T> &>(value);
        decltype(auto) current = typed.value();
        assign_from_value(destination, std::addressof(current));
    }

    [[nodiscard]] std::size_t hash(const void *value) const noexcept override {
        if constexpr (Traits::supports_hash) {
            return Traits::hash(Traits::read(*static_cast<const Stored *>(value)));
        } else {
            std::terminate();
        }
    }

    [[nodiscard]] bool equals(const void *value, const ScalarValueInputView &other) const noexcept override {
        if constexpr (Traits::supports_equality) {
            const auto &typed = static_cast<const TypedScalarValueInputView<T> &>(other);
            return Traits::read(*static_cast<const Stored *>(value)) == typed.value();
        } else {
            std::terminate();
        }
    }

    void destroy(void *value, std::pmr::memory_resource &memory) const noexcept override {
        Traits::destroy(static_cast<Stored *>(value), memory);
    }
};

template <typename T>
class CppScalarStorageRepresentation final : public CppScalarStorageRepresentationBase<T, ScalarStorageRepresentation> {
};

template <typename T>
class CppScalarStorageRepresentation<Opaque<T>> final
    : public CppScalarStorageRepresentationBase<Opaque<T>, ScalarStorageRepresentation> {
  public:
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return false; }

    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return false; }

    void construct_initial_value(void *) const noexcept override { std::terminate(); }

    void copy_construct(void *destination, const void *source, std::pmr::memory_resource &) const override {
        std::construct_at(static_cast<T *>(destination), *static_cast<const T *>(source));
    }

    void assign_from_value(void *, const void *) const override { std::terminate(); }
};

template <typename T>
class CppScalarStorageRepresentation<List<T>> final
    : public CppScalarStorageRepresentationBase<List<T>, ScalarListStorageRepresentation> {
  public:
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_list_storage<T> *>(destination));
    }

    [[nodiscard]] const ScalarStorageRepresentation &item_storage_representation() const noexcept override {
        return cpp_scalar_storage_representation<T>();
    }

    [[nodiscard]] std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_list_storage<T> *>(storage)->items.size();
    }

    [[nodiscard]] const void *item(const void *storage, std::size_t index) const noexcept override {
        const auto &items = static_cast<const details::scalar_list_storage<T> *>(storage)->items;
        return std::addressof(items[static_cast<std::uint32_t>(index)]);
    }

    void append(void *storage, const ScalarValueInputView &value, std::pmr::memory_resource &memory) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        items.emplace_back_with(memory, [&](auto *destination) {
            value.copy_construct(destination, item_storage_representation(), memory);
        });
    }

    void set(void *storage, std::size_t index, const ScalarValueInputView &value) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        value.assign(std::addressof(items[static_cast<std::uint32_t>(index)]), item_storage_representation());
    }

    void erase(void *storage, std::size_t index, std::pmr::memory_resource &memory) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        for (auto position = static_cast<std::uint32_t>(index); position + 1 < items.size(); ++position) {
            items[position] = std::move(items[position + 1]);
        }
        items.pop_back_with(memory, [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

    void clear(void *storage, std::pmr::memory_resource &memory) const noexcept override {
        static_cast<details::scalar_list_storage<T> *>(storage)->items.release_with(
            memory, [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

    [[nodiscard]] const ScalarListMutableInputView *list_mutable_view_prototype() const noexcept override {
        using View = util::polymorphic_value<ScalarListMutableInputView>;
        static const auto prototype = View::template make<details::BorrowedMutableScalarList<T>>();

        return &prototype.get();
    }
};

template <typename T>
class CppScalarStorageRepresentation<Set<T>> final
    : public CppScalarStorageRepresentationBase<Set<T>, ScalarSetStorageRepresentation> {
  public:
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_set_storage<T> *>(destination));
    }

    [[nodiscard]] const ScalarStorageRepresentation &item_storage_representation() const noexcept override {
        return cpp_scalar_storage_representation<T>();
    }

    [[nodiscard]] std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_set_storage<T> *>(storage)->items.size();
    }

    [[nodiscard]] bool contains(const void *storage, const ScalarValueInputView &value) const override {
        const auto &items = static_cast<const details::scalar_set_storage<T> *>(storage)->items;
        return items.contains(typed_scalar_value<T>(value));
    }

    [[nodiscard]] bool insert(void *storage, const ScalarValueInputView &value,
                              std::pmr::memory_resource &memory) const override {
        const auto item = typed_scalar_value<T>(value);
        auto &items = static_cast<details::scalar_set_storage<T> *>(storage)->items;
        return items
            .insert_with(
                item, memory,
                [&](auto *destination) { scalar_storage_traits<T>::construct(destination, item, memory); },
                [&](auto *destination) noexcept { scalar_storage_traits<T>::destroy(destination, memory); })
            .second;
    }

    [[nodiscard]] bool erase(void *storage, const ScalarValueInputView &value,
                             std::pmr::memory_resource &memory) const override {
        auto &items = static_cast<details::scalar_set_storage<T> *>(storage)->items;
        return items.erase_with(typed_scalar_value<T>(value), memory,
                                [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

    void clear(void *storage, std::pmr::memory_resource &memory) const noexcept override {
        static_cast<details::scalar_set_storage<T> *>(storage)->items.release_with(
            memory, [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

    [[nodiscard]] const ScalarSetMutableInputView *set_mutable_view_prototype() const noexcept override {
        using View = util::polymorphic_value<ScalarSetMutableInputView>;
        static const auto prototype = View::template make<details::BorrowedMutableScalarSet<T>>();

        return &prototype.get();
    }
};

template <typename K, typename V>
class CppScalarStorageRepresentation<Dict<K, V>> final
    : public CppScalarStorageRepresentationBase<Dict<K, V>, ScalarDictStorageRepresentation> {
  public:
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_dict_storage<K, V> *>(destination));
    }

    [[nodiscard]] const ScalarStorageRepresentation &key_storage_representation() const noexcept override {
        return cpp_scalar_storage_representation<K>();
    }

    [[nodiscard]] const ScalarStorageRepresentation &mapped_storage_representation() const noexcept override {
        return cpp_scalar_storage_representation<V>();
    }

    [[nodiscard]] std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items.size();
    }

    [[nodiscard]] bool contains(const void *storage, const ScalarValueInputView &key) const override {
        const auto &items = static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items;
        return items.contains(typed_scalar_value<K>(key));
    }

    [[nodiscard]] ScalarDictStorageRepresentation::Item at(void *storage,
                                                           const ScalarValueInputView &key) const override {
        auto &value = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items.at(typed_scalar_value<K>(key));
        return {mapped_storage_representation(), std::addressof(value)};
    }

    void insert_or_assign(void *storage, const ScalarValueInputView &key, const ScalarValueInputView &value,
                          std::pmr::memory_resource &memory) const override {
        const auto key_value = typed_scalar_value<K>(key);
        auto &items = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items;
        auto [stored, inserted] = items.try_emplace_with(
            key_value, memory,
            [&](auto *destination) { scalar_storage_traits<K>::construct(destination, key_value, memory); },
            [&](auto *destination) noexcept { scalar_storage_traits<K>::destroy(destination, memory); },
            [&](auto *destination) { value.copy_construct(destination, mapped_storage_representation(), memory); });
        if (!inserted) {
            value.assign(stored, mapped_storage_representation());
        }
    }

    [[nodiscard]] bool erase(void *storage, const ScalarValueInputView &key,
                             std::pmr::memory_resource &memory) const override {
        auto &items = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items;
        return items.erase_with(
            typed_scalar_value<K>(key), memory,
            [&](auto *item) noexcept { scalar_storage_traits<K>::destroy(item, memory); },
            [&](auto *item) noexcept { scalar_storage_traits<V>::destroy(item, memory); });
    }

    void clear(void *storage, std::pmr::memory_resource &memory) const noexcept override {
        static_cast<details::scalar_dict_storage<K, V> *>(storage)->items.release_with(
            memory, [&](auto *item) noexcept { scalar_storage_traits<K>::destroy(item, memory); },
            [&](auto *item) noexcept { scalar_storage_traits<V>::destroy(item, memory); });
    }

    [[nodiscard]] const ScalarDictMutableInputView *dict_mutable_view_prototype() const noexcept override {
        using View = util::polymorphic_value<ScalarDictMutableInputView>;
        static const auto prototype = View::template make<details::BorrowedMutableScalarDict<K, V>>();

        return &prototype.get();
    }
};

} // namespace bg::details

namespace bg {

template <typename T> const ScalarStorageRepresentation &cpp_scalar_storage_representation() noexcept {
    static const details::CppScalarStorageRepresentation<T> representation;
    static const auto registered = (register_scalar_storage_representation(representation), true);
    (void)registered;

    return representation;
}

template <typename T> void TypedTimeSeriesValueOutputView<T>::set_value(const ScalarInputView &value) const {
    value.assign(mutable_value(), cpp_scalar_storage_representation<T>());
    publish();
}

} // namespace bg
