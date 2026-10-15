#pragma once

#include <bg/scalar/details/cpp_runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/storage_traits.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstddef>
#include <memory>

namespace bg::details {

template <typename T>
class CppScalarRuntimeType<List<T>> final : public CppScalarRuntimeTypeBase<List<T>, ScalarListRuntimeType> {
    using Base = CppScalarRuntimeTypeBase<List<T>, ScalarListRuntimeType>;

    static_assert(util::byte_copy_initializable<details::scalar_list_storage<T>>);

  public:
    CppScalarRuntimeType() : Base{cpp_scalar_type<List<T>>(), cpp_scalar_runtime_type<T>()} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::scalar_list_storage<T> *>(destination));
    }

    void do_construct_from_input(void *destination, const details::ScalarData &value,
                                 allocator &memory) const override {
        construct_initial_value(destination);
        auto rollback = util::scope_exit{[&]() noexcept { this->destroy(destination, memory); }};
        const auto &source_type = static_cast<const ScalarListRuntimeType &>(value.runtime_type());
        for (auto index = std::size_t{}; index < source_type.item_count(value.data()); ++index) {
            append(destination, {source_type.item_runtime_type(), source_type.item(value.data(), index)}, memory);
        }
        rollback.release();
    }

    void do_assign_from_input(void *destination, const details::ScalarData &value) const override {
        auto input = value;
        const auto construct = [](void *context, const ScalarRuntimeType &runtime_type, void *target,
                                  allocator &memory) {
            runtime_type.construct_from_input(target, *static_cast<const details::ScalarData *>(context), memory);
        };
        (void)assign_with(destination, {&input, construct}, false);
    }

    bool equals_same_type(const void *storage, const details::ScalarData &other) const override {
        const auto &source_type = static_cast<const ScalarListRuntimeType &>(other.runtime_type());
        const auto &items = static_cast<const details::scalar_list_storage<T> *>(storage)->items;
        if (items.size() != source_type.item_count(other.data())) {
            return false;
        }

        for (auto index = std::uint32_t{}; index < items.size(); ++index) {
            const auto item = ScalarData{source_type.item_runtime_type(), source_type.item(other.data(), index)};
            if (!this->item_runtime_type().equals(std::addressof(items[index]), item)) {
                return false;
            }
        }
        return true;
    }

    std::size_t item_count(const void *storage) const noexcept override {
        return static_cast<const details::scalar_list_storage<T> *>(storage)->items.size();
    }

    const void *item(const void *storage, std::size_t index) const noexcept override {
        const auto &items = static_cast<const details::scalar_list_storage<T> *>(storage)->items;
        return std::addressof(items[static_cast<std::uint32_t>(index)]);
    }

    void *item(void *storage, std::size_t index) const noexcept override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        return std::addressof(items[static_cast<std::uint32_t>(index)]);
    }

    void emplace_back_with(void *storage, allocator &memory, ScalarConstruction construction) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        items.emplace_back_with(memory.memory_resource(), [&](auto *destination) {
            construction.construct(construction.context, this->item_runtime_type(), destination, memory);
        });
    }

    bool assign_with(void *destination, ScalarConstruction construction, bool suppress_equal) const override {
        auto memory = current_scalar_allocator();
        alignas(details::scalar_list_storage<T>) std::byte replacement_bytes[sizeof(details::scalar_list_storage<T>)];
        construction.construct(construction.context, *this, replacement_bytes, memory);
        auto *replacement = std::launder(
            reinterpret_cast<details::scalar_list_storage<T> *>(replacement_bytes));
        auto release_replacement = util::scope_exit{[&]() noexcept { this->destroy(replacement, memory); }};
        const auto source = details::ScalarData{*this, replacement};

        if (suppress_equal && this->ScalarRuntimeType::equals(destination, source)) {
            return false;
        }

        auto &target_items = static_cast<details::scalar_list_storage<T> *>(destination)->items;
        target_items.swap(replacement->items);
        return true;
    }

    bool try_assign_from_owned(void *destination, void *source, allocator &source_memory,
                               allocator &destination_memory) const noexcept override {
        if (!source_memory.memory_resource().is_equal(destination_memory.memory_resource())) {
            return false;
        }

        auto &target_items = static_cast<details::scalar_list_storage<T> *>(destination)->items;
        auto &source_items = static_cast<details::scalar_list_storage<T> *>(source)->items;
        target_items.swap(source_items);
        return true;
    }

    void append(void *storage, const details::ScalarData &value, allocator &memory) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        items.emplace_back_with(
            memory.memory_resource(),
            [&](auto *destination) { value.copy_construct(destination, this->item_runtime_type(), memory); });
    }

    void set(void *storage, std::size_t index, const details::ScalarData &value) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        value.assign(std::addressof(items[static_cast<std::uint32_t>(index)]), this->item_runtime_type());
    }

    void erase(void *storage, std::size_t index, allocator &memory) const override {
        auto &items = static_cast<details::scalar_list_storage<T> *>(storage)->items;
        auto replacement = details::scalar_list_storage<T>{};
        auto rollback = util::scope_exit{[&]() noexcept {
            replacement.items.release_with(
                memory.memory_resource(),
                [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
        }};
        for (auto position = std::uint32_t{}; position < items.size(); ++position) {
            if (position == index) {
                continue;
            }
            replacement.items.emplace_back_with(memory.memory_resource(), [&](auto *item) {
                this->item_runtime_type().copy_construct(item, std::addressof(items[position]), memory);
            });
        }
        items.swap(replacement.items);
    }

    void clear(void *storage, allocator &memory) const noexcept override {
        static_cast<details::scalar_list_storage<T> *>(storage)->items.release_with(
            memory.memory_resource(), [&](auto *item) noexcept { scalar_storage_traits<T>::destroy(item, memory); });
    }

};

} // namespace bg::details
