#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/set/type.hpp>

namespace bg {

template <typename T> class MutableSet : public Set<T> {
    using Base = Set<T>;
    using Storage = details::scalar_set_storage<T>;
    using Traits = details::scalar_storage_traits<T>;

  public:
    explicit MutableSet(Storage &storage) noexcept : Base{storage} {}

    [[nodiscard]] std::size_t size() const noexcept { return Base::size(); }
    [[nodiscard]] bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] bool contains(typename Traits::value_type value) const { return Base::contains(value); }

    [[nodiscard]] bool insert(typename Traits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        return mutable_storage()
            .items
            .insert_with(
                value, memory, [&](auto *stored) { Traits::construct(stored, value, memory); },
                [&](auto *stored) noexcept { Traits::destroy(stored, memory); })
            .second;
    }

    [[nodiscard]] bool erase(typename Traits::value_type value) const {
        auto &memory = details::current_scalar_memory_resource();
        return mutable_storage().items.erase_with(value, memory,
                                                  [&](auto *stored) noexcept { Traits::destroy(stored, memory); });
    }

    void clear() const {
        auto &memory = details::current_scalar_memory_resource();
        mutable_storage().items.release_with(memory, [&](auto *stored) noexcept { Traits::destroy(stored, memory); });
    }

  private:
    [[nodiscard]] Storage &mutable_storage() const noexcept { return const_cast<Storage &>(Base::storage()); }
};

} // namespace bg
