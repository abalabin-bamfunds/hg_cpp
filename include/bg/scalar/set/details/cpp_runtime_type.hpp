#pragma once

#include <bg/scalar/details/cpp_runtime_type.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/set/storage_traits.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>

namespace bg::details {

template <typename T>
class CppScalarRuntimeType<Set<T>> final : public CppScalarRuntimeTypeBase<Set<T>, ScalarSetRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<Set<T>, ScalarSetRuntimeType>;

    static_assert(util::byte_copy_initializable<details::scalar_set_storage<T>>);

  public:
    CppScalarRuntimeType() : Base{cpp_scalar_type<Set<T>>()} {}

    void initialize_runtime_type() { this->initialize_item_runtime_type(cpp_scalar_runtime_type<T>()); }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_set_storage<T> *>(destination));
    }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { clear(destination, memory); }};
        auto copy = SetCopy{this, destination, &memory};
        const auto &source_type = static_cast<const ScalarSetRuntimeType &>(value.runtime_type());
        source_type.for_each(value.data(), &copy, copy_item);
        rollback.release();
    }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto memory = current_scalar_allocator();
        auto replacement = details::scalar_set_storage<T>{};
        auto release_replacement = util::scope_exit{[&]() noexcept { clear(&replacement, memory); }};
        auto copy = SetCopy{this, &replacement, &memory};
        const auto &source_type = static_cast<const ScalarSetRuntimeType &>(value.runtime_type());
        source_type.for_each(value.data(), &copy, copy_item);

        static_cast<details::scalar_set_storage<T> *>(destination)->items.swap(replacement.items);
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = current_scalar_allocator();
        alignas(details::scalar_set_storage<T>) std::byte replacement_bytes[sizeof(details::scalar_set_storage<T>)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(
            reinterpret_cast<details::scalar_set_storage<T> *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { this->destroy(replacement, memory); }};
        const auto source = this->borrow(replacement);

        if (suppress_equal && this->ScalarRuntimeType::equals(destination, source)) {
            return false;
        }

        static_cast<details::scalar_set_storage<T> *>(destination)->items.swap(replacement->items);
        return true;
    }

    bool try_assign_from_owned(void *destination, void *source, allocator &source_memory,
                               allocator &destination_memory) const noexcept override {
        if (!source_memory.memory_resource().is_equal(destination_memory.memory_resource())) {
            return false;
        }

        auto &target_items = static_cast<details::scalar_set_storage<T> *>(destination)->items;
        auto &source_items = static_cast<details::scalar_set_storage<T> *>(source)->items;
        target_items.swap(source_items);
        return true;
    }

    bool equals_same_type(const void *storage, const details::ScalarData &other) const override {
        const auto &source_type = static_cast<const ScalarSetRuntimeType &>(other.runtime_type());
        if (item_count(storage) != source_type.item_count(other.data())) {
            return false;
        }

        auto comparison = SetComparison{this, storage, true};
        source_type.for_each(other.data(), &comparison, compare_item);
        return comparison.equal;
    }

    std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_set_storage<T> *>(storage)->items.size();
    }

    bool contains(const void *storage, const details::ScalarData &value) const override {
        const auto &items = static_cast<const details::scalar_set_storage<T> *>(storage)->items;
        return items.contains_with(value.hash(), [&](const auto &item) {
            return this->item_runtime_type().equals(std::addressof(item), value);
        });
    }

    void for_each(const void *storage, void *context, ScalarSetRuntimeType::ItemFunction function) const override {
        for (const auto &item : static_cast<const details::scalar_set_storage<T> *>(storage)->items) {
            function(context, {this->item_runtime_type(), std::addressof(item)});
        }
    }

    [[nodiscard]] bool insert(void *storage, const details::ScalarData &value,
                              allocator &memory) const override {
        auto &items = static_cast<details::scalar_set_storage<T> *>(storage)->items;
        return items
            .insert_with(
                value.hash(),
                [&](const auto &item) { return this->item_runtime_type().equals(std::addressof(item), value); },
                memory.memory_resource(),
                [&](auto *destination) { value.copy_construct(destination, this->item_runtime_type(), memory); },
                [&](auto *destination) noexcept { scalar_storage_traits<T>::destroy(destination, memory); })
            .second;
    }

    [[nodiscard]] bool insert_with(void *storage, allocator &memory,
                                   ScalarConstruction construction) const override {
        auto &items = static_cast<details::scalar_set_storage<T> *>(storage)->items;
        return items
            .insert_constructed_with(
                memory.memory_resource(),
                [&](const auto &item) { return this->item_runtime_type().hash(std::addressof(item)); },
                [&](const auto &stored, const auto &candidate) {
                    const auto candidate_data = this->item_runtime_type().borrow(std::addressof(candidate));
                    return this->item_runtime_type().equals(std::addressof(stored), candidate_data);
                },
                [&](auto *destination) {
                    construction.construct(construction.context, this->item_runtime_type(), destination, memory);
                },
                [&](auto *destination) noexcept { scalar_storage_traits<T>::destroy(destination, memory); })
            .second;
    }

    [[nodiscard]] bool erase(void *storage, const details::ScalarData &value,
                             allocator &memory) const override {
        auto &items = static_cast<details::scalar_set_storage<T> *>(storage)->items;
        return items.erase_with(
            value.hash(),
            [&](const auto &item) { return this->item_runtime_type().equals(std::addressof(item), value); },
            memory.memory_resource(), [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

    void clear(void *storage, allocator &memory) const noexcept override {
        static_cast<details::scalar_set_storage<T> *>(storage)->items.release_with(
            memory.memory_resource(), [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

  private:
    struct SetCopy {
        const CppScalarRuntimeType *runtime_type;
        void *storage;
        allocator *memory;
    };

    struct SetComparison {
        const CppScalarRuntimeType *runtime_type;
        const void *storage;
        bool equal;
    };

    static void copy_item(void *context, ScalarSetRuntimeType::Item item) {
        auto &copy = *static_cast<SetCopy *>(context);
        const auto inserted = copy.runtime_type->insert(copy.storage, {item.runtime_type, item.data}, *copy.memory);
        assert(inserted);
    }

    static void compare_item(void *context, ScalarSetRuntimeType::Item item) {
        auto &comparison = *static_cast<SetComparison *>(context);
        comparison.equal &= comparison.runtime_type->contains(comparison.storage, {item.runtime_type, item.data});
    }
};

} // namespace bg::details
