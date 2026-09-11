#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/details/data.hpp>
#include <bg/scalar/details/storage_traits_base.hpp>

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace bg::details {

template <typename Source, typename T>
concept scalar_view_source_for = requires(const Source &source) {
    typename std::remove_cvref_t<Source>::scalar_type;
    requires std::same_as<typename std::remove_cvref_t<Source>::scalar_type, T>;
    scalar_data(source);
};

template <typename Collection>
void replace_collection_storage(scalar_stored_t<Collection> &destination, scalar_value_t<Collection> value,
                                allocator &memory) {
    using Storage = scalar_stored_t<Collection>;
    using Traits = scalar_storage_traits<Collection>;

    alignas(Storage) std::byte replacement_storage[sizeof(Storage)];
    auto *replacement = reinterpret_cast<Storage *>(replacement_storage);
    Traits::construct(replacement, value, memory);

    destination.items.swap(replacement->items);
    Traits::destroy(replacement, memory);
}

} // namespace bg::details
