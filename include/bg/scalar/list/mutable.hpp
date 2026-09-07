#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/mutable_traits.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/list/type.hpp>

#include <cstddef>
#include <cstdint>

namespace bg {

template <typename T> class MutableList : public List<T> {
    using Base = List<T>;
    using Storage = details::scalar_list_storage<T>;
    using Traits = details::scalar_storage_traits<T>;

  public:
    explicit MutableList(Storage &storage) noexcept : Base{storage} {}

    std::size_t size() const noexcept { return Base::size(); }
    bool empty() const noexcept { return Base::empty(); }
    decltype(auto) operator[](std::size_t index) const noexcept;

    void append(typename Traits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        mutable_storage().items.emplace_back_with(memory,
                                                  [&](auto *stored) { Traits::construct(stored, value, memory); });
    }

    void set(std::size_t index, typename Traits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        Traits::assign(mutable_storage().items[static_cast<std::uint32_t>(index)], value, memory);
    }

    void erase(std::size_t index) const {
        auto &memory = details::current_scalar_memory_resource();
        auto &items = mutable_storage().items;

        for (auto i = static_cast<std::uint32_t>(index); i + 1 < items.size(); ++i) {
            items[i] = std::move(items[i + 1]);
        }
        items.pop_back_with(memory, [&](auto *item) noexcept { Traits::destroy(item, memory); });
    }

    void clear() const {
        auto &memory = details::current_scalar_memory_resource();
        mutable_storage().items.release_with(memory, [&](auto *item) noexcept { Traits::destroy(item, memory); });
    }

  private:
    // MutableList is constructed from mutable storage; List retains the same pointer for read-only access.
    Storage &mutable_storage() const noexcept { return const_cast<Storage &>(Base::storage()); }
};

} // namespace bg

namespace bg::details {

template <typename T>
    requires(!generic_scalar<List<T>>)
struct scalar_mutable_traits<List<T>> {
    using storage_type = scalar_list_storage<T>;
    using view_type = MutableList<T>;
};

} // namespace bg::details
