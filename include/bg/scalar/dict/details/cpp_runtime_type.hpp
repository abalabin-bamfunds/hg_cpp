#pragma once

#include <bg/scalar/details/cpp_runtime_type.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/storage_traits.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/scope_exit.hpp>

#include <stdexcept>

namespace bg::details {

template <typename K, typename V>
class CppScalarRuntimeType<Dict<K, V>> final : public CppScalarRuntimeTypeBase<Dict<K, V>, ScalarDictRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<Dict<K, V>, ScalarDictRuntimeType>;

    static_assert(util::byte_copy_initializable<details::scalar_dict_storage<K, V>>);

  public:
    CppScalarRuntimeType()
        : Base{cpp_scalar_type<Dict<K, V>>(), cpp_scalar_runtime_type<K>(), cpp_scalar_runtime_type<V>()} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_dict_storage<K, V> *>(destination));
    }

    void construct_from_input(void *destination, const details::ScalarData &value,
                              allocator &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { clear(destination, memory); }};
        auto copy = DictCopy{this, destination, &memory};
        const auto &source_type = static_cast<const ScalarDictRuntimeType &>(value.runtime_type());
        source_type.for_each(value.data(), &copy, copy_entry);
        rollback.release();
    }

    void assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto memory = current_scalar_allocator();
        auto replacement = details::scalar_dict_storage<K, V>{};
        auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = DictCopy{this, &replacement, &memory};
        const auto &source_type = static_cast<const ScalarDictRuntimeType &>(value.runtime_type());
        source_type.for_each(value.data(), &copy, copy_entry);

        static_cast<details::scalar_dict_storage<K, V> *>(destination)->items.swap(replacement.items);
    }

    bool equals_same_type(const void *storage, const details::ScalarData &other) const noexcept override {
        const auto &source_type = static_cast<const ScalarDictRuntimeType &>(other.runtime_type());
        if (item_count(storage) != source_type.item_count(other.data())) {
            return false;
        }

        auto comparison = DictComparison{this, storage, true};
        source_type.for_each(other.data(), &comparison, compare_entry);
        return comparison.equal;
    }

    std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items.size();
    }

    bool contains(const void *storage, const details::ScalarData &key) const override {
        const auto &items = static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items;
        return items.find_with(key.hash(), [&](const auto &stored) {
            return this->key_runtime_type().equals(std::addressof(stored), key);
        }) != nullptr;
    }

    ScalarDictRuntimeType::ConstItem at(const void *storage,
                                                      const details::ScalarData &key) const override {
        const auto &items = static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items;
        const auto *value = items.find_with(key.hash(), [&](const auto &stored) {
            return this->key_runtime_type().equals(std::addressof(stored), key);
        });
        if (value == nullptr) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return {this->mapped_runtime_type(), value};
    }

    ScalarDictRuntimeType::Item at(void *storage, const details::ScalarData &key) const override {
        auto &items = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items;
        auto *value = items.find_with(key.hash(), [&](const auto &stored) {
            return this->key_runtime_type().equals(std::addressof(stored), key);
        });
        if (value == nullptr) {
            throw std::out_of_range{"dictionary key was not found"};
        }
        return {this->mapped_runtime_type(), value};
    }

    void for_each(const void *storage, void *context, ScalarDictRuntimeType::EntryFunction function) const override {
        for (const auto [key, value] : static_cast<const details::scalar_dict_storage<K, V> *>(storage)->items) {
            function(context, {{this->key_runtime_type(), std::addressof(key)},
                               {this->mapped_runtime_type(), std::addressof(value)}});
        }
    }

    void insert_or_assign(void *storage, const details::ScalarData &key, const details::ScalarData &value,
                          allocator &memory) const override {
        auto &items = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items;
        auto [stored, inserted] = items.try_emplace_with(
            key.hash(),
            [&](const auto &stored) { return this->key_runtime_type().equals(std::addressof(stored), key); },
            memory.memory_resource(),
            [&](auto *destination) { key.copy_construct(destination, this->key_runtime_type(), memory); },
            [&](auto *destination) noexcept { scalar_storage_traits<K>::destroy(destination, memory); },
            [&](auto *destination) { value.copy_construct(destination, this->mapped_runtime_type(), memory); });
        if (!inserted) {
            value.assign(stored, this->mapped_runtime_type());
        }
    }

    [[nodiscard]] bool erase(void *storage, const details::ScalarData &key,
                             allocator &memory) const override {
        auto &items = static_cast<details::scalar_dict_storage<K, V> *>(storage)->items;
        return items.erase_with(
            key.hash(),
            [&](const auto &stored) { return this->key_runtime_type().equals(std::addressof(stored), key); },
            memory.memory_resource(), [&](auto *item) noexcept { scalar_storage_traits<K>::destroy(item, memory); },
            [&](auto *item) noexcept { scalar_storage_traits<V>::destroy(item, memory); });
    }

    void clear(void *storage, allocator &memory) const noexcept override {
        static_cast<details::scalar_dict_storage<K, V> *>(storage)->items.release_with(
            memory.memory_resource(), [&](auto *item) noexcept { scalar_storage_traits<K>::destroy(item, memory); },
            [&](auto *item) noexcept { scalar_storage_traits<V>::destroy(item, memory); });
    }

  private:
    struct DictCopy {
        const CppScalarRuntimeType *runtime_type;
        void *storage;
        allocator *memory;
    };

    struct DictComparison {
        const CppScalarRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static void copy_entry(void *context, ScalarDictRuntimeType::Entry entry) {
        auto &copy = *static_cast<DictCopy *>(context);
        copy.runtime_type->insert_or_assign(copy.storage, {entry.key.runtime_type, entry.key.data},
                                            {entry.value.runtime_type, entry.value.data}, *copy.memory);
    }

    static void compare_entry(void *context, ScalarDictRuntimeType::Entry entry) {
        auto &comparison = *static_cast<DictComparison *>(context);
        const auto key = details::ScalarData{entry.key.runtime_type, entry.key.data};
        const auto mapped = details::ScalarData{entry.value.runtime_type, entry.value.data};
        const auto &items = static_cast<const details::scalar_dict_storage<K, V> *>(comparison.storage)->items;
        const auto *stored = items.find_with(key.hash(), [&](const auto &candidate) {
            return comparison.runtime_type->key_runtime_type().equals(std::addressof(candidate), key);
        });
        comparison.equal &= stored != nullptr && comparison.runtime_type->mapped_runtime_type().equals(stored, mapped);
    }
};

} // namespace bg::details
