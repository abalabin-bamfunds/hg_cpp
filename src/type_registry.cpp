#include <bg/type/type_registry.hpp>

#include <algorithm>
#include <utility>

namespace bg {

TypeRegistry &TypeRegistry::instance() noexcept {
    static auto registry = TypeRegistry{};
    return registry;
}

void TypeRegistry::register_static_type(const Type &type) noexcept {
    type.registry_entry().link(instance().static_entries_);
}

void TypeRegistry::register_owned_type(std::unique_ptr<Type> type) {
    const auto lock = std::lock_guard{mutex_};
    owned_types_.push_back(std::move(type));
}

void TypeRegistry::rollback(const Type &type) noexcept {
    auto removed = std::unique_ptr<Type>{};
    {
        const auto lock = std::lock_guard{mutex_};
        const auto found = std::ranges::find(owned_types_, &type, [](const auto &candidate) { return candidate.get(); });
        if (found == owned_types_.end()) {
            return;
        }

        removed = std::move(*found);
        owned_types_.erase(found);
    }
}

bool TypeRegistry::contains(const Type &type) const {
    for (auto *entry = static_entries_.load(std::memory_order_acquire); entry != nullptr; entry = entry->next()) {
        if (&entry->object() == &type) {
            return true;
        }
    }

    const auto lock = std::lock_guard{mutex_};
    return std::ranges::any_of(owned_types_, [&](const auto &candidate) { return candidate.get() == &type; });
}

} // namespace bg
