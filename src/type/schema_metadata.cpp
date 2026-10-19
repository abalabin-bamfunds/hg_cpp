#include <bg/type/schema_metadata_values.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bg {

struct SchemaMetadata::Impl {
    struct Entry {
        std::string name;
        ScalarValue<> value;
    };

    std::vector<Entry> entries;
};

bool SchemaMetadata::empty() const noexcept {
    return impl_ == nullptr || impl_->entries.empty();
}

std::size_t SchemaMetadata::size() const noexcept {
    return impl_ == nullptr ? 0 : impl_->entries.size();
}

std::size_t SchemaMetadata::hash() const noexcept {
    auto result = std::size_t{};
    if (impl_ == nullptr) {
        return result;
    }

    for (const auto &entry : impl_->entries) {
        const auto value = std::hash<std::string_view>{}(entry.name);
        result ^= value + 0x9e3779b9 + (result << 6) + (result >> 2);
    }
    return result;
}

const ScalarData *SchemaMetadata::find(std::string_view name) const noexcept {
    if (impl_ == nullptr) {
        return nullptr;
    }

    const auto found = std::ranges::lower_bound(impl_->entries, name, {}, &Impl::Entry::name);
    if (found == impl_->entries.end() || found->name != name) {
        return nullptr;
    }
    return std::addressof(details::scalar_data(found->value));
}

SchemaMetadata SchemaMetadata::with_overrides(const SchemaMetadata &overrides) const {
    if (overrides.empty()) {
        return *this;
    }
    if (empty()) {
        return overrides;
    }

    auto entries = std::vector<SchemaMetadataEntry>{};
    entries.reserve(size() + overrides.size());

    auto base = impl_->entries.begin();
    auto override = overrides.impl_->entries.begin();
    while (base != impl_->entries.end() && override != overrides.impl_->entries.end()) {
        if (base->name < override->name) {
            entries.push_back({base->name, details::scalar_data(base->value)});
            ++base;
            continue;
        }
        if (override->name < base->name) {
            entries.push_back({override->name, details::scalar_data(override->value)});
            ++override;
            continue;
        }

        entries.push_back({override->name, details::scalar_data(override->value)});
        ++base;
        ++override;
    }

    while (base != impl_->entries.end()) {
        entries.push_back({base->name, details::scalar_data(base->value)});
        ++base;
    }
    while (override != overrides.impl_->entries.end()) {
        entries.push_back({override->name, details::scalar_data(override->value)});
        ++override;
    }

    return make_schema_metadata(entries);
}

bool SchemaMetadata::operator==(const SchemaMetadata &other) const {
    if (impl_ == other.impl_) {
        return true;
    }
    if (size() != other.size()) {
        return false;
    }

    if (impl_ == nullptr) {
        return true;
    }

    for (auto index = std::size_t{}; index < impl_->entries.size(); ++index) {
        const auto &left = impl_->entries[index];
        const auto &right = other.impl_->entries[index];
        if (left.name != right.name || !left.value.equals(right.value)) {
            return false;
        }
    }
    return true;
}

SchemaMetadata make_schema_metadata(std::span<const SchemaMetadataEntry> entries) {
    if (entries.empty()) {
        return {};
    }

    auto implementation = std::make_shared<SchemaMetadata::Impl>();
    implementation->entries.reserve(entries.size());
    for (const auto &entry : entries) {
        if (entry.value.empty()) {
            throw std::invalid_argument{"schema metadata values must be present scalar values"};
        }
        if (!entry.value.type().supports_equality()) {
            throw std::invalid_argument{"schema metadata values must support equality"};
        }

        auto value = ScalarValue<>{Scalar<>::from(entry.value)};
        if (!value.equals(value)) {
            throw std::invalid_argument{"schema metadata values must compare equal to themselves"};
        }
        implementation->entries.push_back({std::string{entry.name}, std::move(value)});
    }

    std::ranges::sort(implementation->entries, {}, &SchemaMetadata::Impl::Entry::name);
    for (auto index = std::size_t{1}; index < implementation->entries.size(); ++index) {
        if (implementation->entries[index - 1].name == implementation->entries[index].name) {
            throw std::invalid_argument{"schema metadata has duplicate names"};
        }
    }

    return SchemaMetadata{std::move(implementation)};
}

} // namespace bg
