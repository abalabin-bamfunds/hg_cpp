#pragma once

#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/mutable_traits.hpp>
#include <bg/scalar/details/storage_traits.hpp>
#include <bg/scalar/set/type.hpp>

namespace bg {

template <typename T> class MutableSet : public Set<T> {
    using Base = Set<T>;
    using Storage = details::scalar_set_storage<T>;
    using Traits = details::scalar_storage_traits<T>;

  public:
    explicit MutableSet(Storage &storage) noexcept : Base{storage} {}

    std::size_t size() const noexcept { return Base::size(); }
    bool empty() const noexcept { return Base::empty(); }
    bool contains(typename Traits::value_type value) const { return Base::contains(value); }

    [[nodiscard]] bool insert(typename Traits::value_type value) const {
        auto memory = details::current_scalar_allocator();
        return mutable_storage()
            .items
            .insert_with(
                value, memory, [&](auto *stored) { Traits::construct(stored, value, memory); },
                [&](auto *stored) noexcept { Traits::destroy(stored, memory); })
            .second;
    }

    [[nodiscard]] bool erase(typename Traits::value_type value) const {
        auto memory = details::current_scalar_allocator();
        return mutable_storage().items.erase_with(value, memory,
                                                  [&](auto *stored) noexcept { Traits::destroy(stored, memory); });
    }

    void clear() const {
        auto memory = details::current_scalar_allocator();
        mutable_storage().items.release_with(memory, [&](auto *stored) noexcept { Traits::destroy(stored, memory); });
    }

  private:
    // MutableSet is constructed from mutable storage; Set retains the same pointer for read-only access.
    Storage &mutable_storage() const noexcept { return const_cast<Storage &>(Base::storage()); }
};

} // namespace bg

namespace bg::details {

template <typename T>
    requires(!generic_scalar<Set<T>>)
struct scalar_mutable_traits<Set<T>> {
    using storage_type = scalar_set_storage<T>;
    using view_type = MutableSet<T>;
};

} // namespace bg::details
