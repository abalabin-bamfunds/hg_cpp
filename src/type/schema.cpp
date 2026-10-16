#include <bg/type/schema.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>

#include "type_registration.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace bg {
namespace {

void cache_reserved_schema(const Schema &schema);
void uncache_reserved_schema(const Schema &schema) noexcept;

} // namespace

struct SchemaRegistration::Impl {
    std::unique_lock<std::recursive_mutex> lock{details::semantic_type_registration_mutex()};
    details::SemanticTypeRegistration registration;
    bool committed{};
};

SchemaRegistration::SchemaRegistration() {
    if (details::SemanticTypeRegistration::current() != nullptr) {
        throw std::logic_error{"schema registrations cannot be nested"};
    }
    impl_ = std::make_unique<Impl>();
}
SchemaRegistration::~SchemaRegistration() = default;

Schema &SchemaRegistration::reserve_schema(util::symbol name, SchemaIdentity identity, const Schema *base) {
    if (impl_->committed) {
        throw std::logic_error{"a committed schema registration cannot reserve descriptors"};
    }
    auto descriptor = std::make_unique<Schema>(name, std::move(identity), base);
    auto *result = descriptor.get();
    TypeRegistry::instance().register_owned(std::move(descriptor));
    auto rollback = util::scope_exit{[result]() noexcept { TypeRegistry::instance().rollback(*result); }};
    impl_->registration.on_rollback([result] {
        uncache_reserved_schema(*result);
        TypeRegistry::instance().rollback(*result);
    });
    impl_->registration.on_commit([result] { cache_reserved_schema(*result); });
    rollback.release();
    return *result;
}

const Type &SchemaRegistration::register_owned(std::unique_ptr<Type> type) {
    if (impl_->committed) {
        throw std::logic_error{"a committed schema registration cannot own more descriptors"};
    }
    const auto &descriptor = TypeRegistry::instance().register_owned(std::move(type));
    auto rollback = util::scope_exit{[&descriptor]() noexcept { TypeRegistry::instance().rollback(descriptor); }};
    impl_->registration.on_rollback([&descriptor] { TypeRegistry::instance().rollback(descriptor); });
    rollback.release();
    return descriptor;
}

void SchemaRegistration::commit() {
    if (impl_->committed) {
        throw std::logic_error{"a schema registration can be committed only once"};
    }
    impl_->registration.commit();
    impl_->committed = true;
}

namespace {

struct SchemaKey {
    util::symbol name;
    SchemaIdentity identity;
    const Schema *base;
    std::vector<SchemaField> direct_fields;

    bool operator==(const SchemaKey &) const noexcept = default;
};

void combine_hash(std::size_t &seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct SchemaKeyHash {
    std::size_t operator()(const SchemaKey &key) const noexcept {
        auto result = std::size_t{};
        combine_hash(result, std::hash<std::string_view>{}(key.name.text()));
        combine_hash(result, std::visit(
                                 [](const auto &identity) -> std::size_t {
                                     using Identity = std::remove_cvref_t<decltype(identity)>;
                                     if constexpr (std::same_as<Identity, std::monostate>) {
                                         return 0;
                                     } else if constexpr (std::same_as<Identity, std::type_index>) {
                                         return std::hash<std::type_index>{}(identity);
                                     } else {
                                         return std::hash<std::uint64_t>{}(identity.value());
                                     }
                                 },
                                 key.identity));
        combine_hash(result, std::hash<const Schema *>{}(key.base));
        for (const auto &field : key.direct_fields) {
            combine_hash(result, std::hash<std::string_view>{}(field.name.text()));
            combine_hash(result, std::hash<const Type *>{}(field.type));
            combine_hash(result, std::hash<bool>{}(field.optional));
        }
        return result;
    }
};

using SchemaCache = std::unordered_map<SchemaKey, const Schema *, SchemaKeyHash>;

SchemaCache &schema_cache() {
    static auto cache = SchemaCache{};
    return cache;
}

void cache_reserved_schema(const Schema &schema) {
    if (!schema.is_finalized()) {
        throw std::logic_error{"a reserved schema must be finalized before commit"};
    }
    auto key = SchemaKey{schema.name(), schema.identity(), schema.base(),
                         {schema.direct_fields().begin(), schema.direct_fields().end()}};
    auto &cache = schema_cache();
    const auto [entry, inserted] = cache.try_emplace(std::move(key), &schema);
    if (!inserted && entry->second != &schema) {
        throw std::logic_error{"a schema registration conflicts with an existing canonical descriptor"};
    }
}

void uncache_reserved_schema(const Schema &schema) noexcept {
    auto &cache = schema_cache();
    std::erase_if(cache, [&schema](const auto &entry) { return entry.second == &schema; });
}

} // namespace

Schema::Schema(util::symbol name, SchemaIdentity identity, const Schema *base, std::vector<SchemaField> fields)
    : name_{name}, identity_{std::move(identity)}, base_{base} {
    finalize(fields);
}

Schema::Schema(util::symbol name, SchemaIdentity identity, const Schema *base)
    : name_{name}, identity_{std::move(identity)}, base_{base} {}

void Schema::finalize(std::span<const SchemaField> fields) {
    if (finalized_) {
        throw std::logic_error{"a schema descriptor can be finalized only once"};
    }
    const auto *registration = details::SemanticTypeRegistration::current();
    if (TypeRegistry::instance().contains(*this) &&
        (registration == nullptr || registration->is_committed())) {
        throw std::logic_error{"finalizing a reserved schema requires its active schema registration"};
    }

    auto direct_fields = std::vector<SchemaField>{fields.begin(), fields.end()};
    auto resolved_fields = std::vector<SchemaField>{};
    for (auto index = std::size_t{}; index < direct_fields.size(); ++index) {
        const auto &field = direct_fields[index];
        if (field.type == nullptr) {
            throw std::invalid_argument{"a schema field requires a type"};
        }
        const auto remaining_fields = std::span{direct_fields}.subspan(index + 1);
        if (std::ranges::find(remaining_fields, field.name, &SchemaField::name) != remaining_fields.end()) {
            throw std::invalid_argument{"a schema has duplicate field names"};
        }

        const auto *inherited = base_ == nullptr ? nullptr : base_->find(field.name);
        if (inherited == nullptr) {
            continue;
        }
        if (!inherited->optional && field.optional) {
            throw std::invalid_argument{"a required schema field cannot become optional"};
        }
        if (inherited->type->is_resolved() && field.type->is_resolved() &&
            !inherited->type->accepts(*field.type)) {
            throw std::invalid_argument{"a redefined schema field must have a compatible narrower type"};
        }
    }

    if (base_ != nullptr) {
        resolved_fields.assign(base_->fields().begin(), base_->fields().end());
    }

    for (const auto &field : direct_fields) {
        const auto inherited = std::ranges::find(resolved_fields, field.name, &SchemaField::name);
        if (inherited == resolved_fields.end()) {
            resolved_fields.push_back(field);
        } else {
            *inherited = field;
        }
    }

    direct_fields_ = std::move(direct_fields);
    fields_ = std::move(resolved_fields);
    finalized_ = true;
}

const SchemaField *Schema::find(util::symbol name) const noexcept {
    const auto field = std::ranges::find(fields_, name, &SchemaField::name);
    return field == fields_.end() ? nullptr : std::addressof(*field);
}

bool Schema::derives_from(const Schema &schema) const noexcept {
    if (!schema.named()) {
        return false;
    }
    for (auto current = this; current != nullptr; current = current->base()) {
        if (current->identity() == schema.identity()) {
            return true;
        }
    }
    return false;
}

bool Schema::is_resolved() const noexcept {
    return finalized_ && (base_ == nullptr || base_->is_resolved()) &&
           std::ranges::all_of(direct_fields_, [](const auto &field) { return field.type->is_resolved(); });
}

const Schema &Schema::resolve(const TypeMap &type_map) const {
    if (!finalized_) {
        throw std::logic_error{"a reserved schema must be finalized before type resolution"};
    }
    if (is_resolved()) {
        return *this;
    }

    auto fields = std::vector<SchemaField>{};
    fields.reserve(direct_fields_.size());
    for (const auto &field : direct_fields_) {
        fields.push_back({field.name, &field.type->resolve(type_map), field.optional});
    }

    const auto *resolved_base = base_ == nullptr ? nullptr : &base_->resolve(type_map);
    return bg::schema(name_, identity_, resolved_base, fields);
}

RuntimeSchemaId new_runtime_schema_id() {
    static auto next_id = std::atomic<std::uint64_t>{1};
    auto id = next_id.load(std::memory_order_relaxed);
    while (id != std::numeric_limits<std::uint64_t>::max()) {
        if (next_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
            return RuntimeSchemaId{id};
        }
    }
    throw std::overflow_error{"runtime schema identity space is exhausted"};
}

const Schema &schema(util::symbol name, SchemaIdentity identity, const Schema *base,
                     std::span<const SchemaField> fields) {
    auto key = SchemaKey{name, std::move(identity), base, {fields.begin(), fields.end()}};
    const auto lock = std::lock_guard{details::semantic_type_registration_mutex()};
    auto &cache = schema_cache();
    if (const auto found = cache.find(key); found != cache.end()) {
        return *found->second;
    }

    const auto &descriptor = TypeRegistry::instance().register_owned(
        std::make_unique<Schema>(key.name, key.identity, key.base, key.direct_fields));
    auto remove_cache_entry = false;
    auto rollback_descriptor = util::scope_exit{[&]() noexcept {
        if (remove_cache_entry) {
            cache.erase(key);
        }
        TypeRegistry::instance().rollback(descriptor);
    }};

    cache.emplace(key, &descriptor);
    remove_cache_entry = true;

    if (auto *registration = details::SemanticTypeRegistration::current()) {
        registration->on_rollback([&cache, key, descriptor = &descriptor] {
            cache.erase(key);
            TypeRegistry::instance().rollback(*descriptor);
        });
    }
    rollback_descriptor.release();
    return descriptor;
}

const Schema &anonymous_schema(std::span<const SchemaField> fields) {
    return schema(util::symbol{""}, std::monostate{}, nullptr, fields);
}

} // namespace bg
