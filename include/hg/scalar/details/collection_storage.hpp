#pragma once

#include <hg/allocator.hpp>
#include <hg/scalar/details/data.hpp>
#include <hg/scalar/storage_traits.hpp>

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace hg::details {

template <typename Source>
concept scalar_data_source = requires(const Source &source) { scalar_data(source); };

template <typename Source, typename T>
concept scalar_view_source_for = scalar_data_source<Source> && requires {
    typename std::remove_cvref_t<Source>::scalar_type;
    requires std::same_as<typename std::remove_cvref_t<Source>::scalar_type, T>;
};

template <typename Collection>
void replace_collection_storage(scalar_stored_t<Collection> &destination,
                                typename scalar_storage_traits<Collection>::value_type value, allocator &memory) {
    using Storage = scalar_stored_t<Collection>;
    using Traits = scalar_storage_traits<Collection>;

    alignas(Storage) std::byte replacement_storage[sizeof(Storage)];
    auto *replacement = reinterpret_cast<Storage *>(replacement_storage);
    Traits::construct(replacement, value, memory);

    destination.items.swap(replacement->items);
    Traits::destroy(replacement, memory);
}

} // namespace hg::details
