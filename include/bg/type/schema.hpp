#pragma once

#include <bg/type/type.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <typeindex>
#include <variant>
#include <vector>

namespace bg {

class SchemaType : public Type {
  public:
    virtual std::size_t field_count() const noexcept { return std::dynamic_extent; }
};

// Engine and extension descriptor. Runtime-defined schemas use BG-allocated identities.
class RuntimeSchemaId {
  public:
    bool operator==(const RuntimeSchemaId &) const noexcept = default;
    std::uint64_t value() const noexcept { return value_; }

  private:
    explicit RuntimeSchemaId(std::uint64_t value) noexcept : value_{value} {}

    friend RuntimeSchemaId new_runtime_schema_id();

    std::uint64_t value_;
};

using SchemaIdentity = std::variant<std::monostate, std::type_index, RuntimeSchemaId>;

struct SchemaField {
    util::symbol name;
    const Type *type;
    bool optional{};

    bool operator==(const SchemaField &) const noexcept = default;
};

class Schema final : public SchemaType {
  public:
    Schema(util::symbol name, SchemaIdentity identity, const Schema *base, std::vector<SchemaField> fields);

    util::symbol name() const noexcept { return name_; }
    const SchemaIdentity &identity() const noexcept { return identity_; }
    bool named() const noexcept { return !std::holds_alternative<std::monostate>(identity_); }
    const Schema *base() const noexcept { return base_; }
    std::span<const SchemaField> direct_fields() const noexcept { return direct_fields_; }
    std::span<const SchemaField> fields() const noexcept { return fields_; }
    std::size_t field_count() const noexcept override { return fields_.size(); }
    const SchemaField &field(std::size_t index) const { return fields_.at(index); }
    const SchemaField *find(util::symbol name) const noexcept;
    bool derives_from(const Schema &schema) const noexcept;

    bool is_resolved() const noexcept override;
    [[nodiscard]] const Schema &resolve(const TypeMap &type_map) const override;

  private:
    util::symbol name_;
    SchemaIdentity identity_;
    const Schema *base_;
    std::vector<SchemaField> direct_fields_;
    std::vector<SchemaField> fields_;
};

// Engine and extension APIs for runtime schema identity and canonical descriptors.
[[nodiscard]] RuntimeSchemaId new_runtime_schema_id();
[[nodiscard]] const Schema &schema(util::symbol name, SchemaIdentity identity, const Schema *base,
                                   std::span<const SchemaField> fields);
[[nodiscard]] const Schema &anonymous_schema(std::span<const SchemaField> fields);

} // namespace bg
