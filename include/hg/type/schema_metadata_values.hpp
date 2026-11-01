#pragma once

#include <hg/scalar/scalar.hpp>
#include <hg/scalar/value.hpp>
#include <hg/type/schema_metadata.hpp>

#include <initializer_list>
#include <span>
#include <string_view>

namespace hg {

struct SchemaMetadataEntry {
    // The factory copies this borrowed value before returning.
    std::string_view name;
    ScalarData value;
};

[[nodiscard]] HG_EXPORT SchemaMetadata make_schema_metadata(std::span<const SchemaMetadataEntry> entries);

[[nodiscard]] inline SchemaMetadata make_schema_metadata(std::initializer_list<SchemaMetadataEntry> entries) {
    return make_schema_metadata(std::span{entries.begin(), entries.size()});
}

template <typename T>
[[nodiscard]] SchemaMetadataEntry schema_metadata_entry(std::string_view name, const Scalar<T> &value) noexcept {
    return {name, details::scalar_data(value)};
}

template <typename T>
[[nodiscard]] SchemaMetadataEntry schema_metadata_entry(std::string_view name,
                                                         const ScalarValue<T> &value) noexcept {
    return {name, details::scalar_data(value)};
}

} // namespace hg
