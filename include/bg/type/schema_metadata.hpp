#pragma once

#include <bg/export.hpp>

#include <cstddef>
#include <concepts>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg {

class ScalarData;
class SchemaMetadata;
struct SchemaMetadataEntry;

// Immutable scalar metadata attached to a schema declaration.
class BG_EXPORT SchemaMetadata {
  public:
    SchemaMetadata() noexcept = default;

    bool empty() const noexcept;
    std::size_t size() const noexcept;
    std::size_t hash() const noexcept;
    const ScalarData *find(std::string_view name) const noexcept;
    [[nodiscard]] SchemaMetadata with_overrides(const SchemaMetadata &overrides) const;
    bool operator==(const SchemaMetadata &other) const;

  private:
    struct Impl;
    explicit SchemaMetadata(std::shared_ptr<const Impl> impl) noexcept : impl_{std::move(impl)} {}

    std::shared_ptr<const Impl> impl_;

    friend SchemaMetadata make_schema_metadata(std::span<const SchemaMetadataEntry> entries);
};

namespace details {

template <template <template <typename, auto...> typename> typename Schema,
          template <typename, auto...> typename Field>
[[nodiscard]] SchemaMetadata declared_schema_metadata() {
    if constexpr (requires { Schema<Field>::metadata(); }) {
        static_assert(std::same_as<std::remove_cvref_t<decltype(Schema<Field>::metadata())>, SchemaMetadata>,
                      "a schema metadata() function must return SchemaMetadata");
        return Schema<Field>::metadata();
    } else {
        return {};
    }
}

} // namespace details

} // namespace bg
