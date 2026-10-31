#include <bg/type/type_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>

namespace bg {

TypeRegistry &TypeRegistry::instance() noexcept {
    alignas(TypeRegistry) static std::byte storage[sizeof(TypeRegistry)];
    static auto *registry = ::new (static_cast<void *>(storage)) TypeRegistry{};
    return *registry;
}

void TypeRegistry::register_static_type(const Type &type) noexcept {
    type.registry_entry().link(instance().static_entries_);
}

void TypeRegistry::register_owned_type(std::unique_ptr<Type> type) {
    owned_types_.push_back(std::move(type));
}

bool TypeRegistry::contains(const Type &type) const {
    for (auto *entry = static_entries_; entry != nullptr; entry = entry->next()) {
        if (&entry->object() == &type) {
            return true;
        }
    }

    return std::ranges::any_of(owned_types_, [&](const auto &candidate) { return candidate.get() == &type; });
}

} // namespace bg
