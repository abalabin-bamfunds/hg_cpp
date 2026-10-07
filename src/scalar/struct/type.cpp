#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/type/type_registry.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace bg {
namespace {

class ResolvedStructScalarType final : public StructScalarType {
  public:
    ResolvedStructScalarType(util::symbol full_name, util::symbol name, std::size_t size, std::size_t alignment,
                             std::vector<StructScalarField> fields)
        : StructScalarType{full_name, name, size, alignment, std::move(fields)} {}

    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
};

} // namespace

namespace details {

const StructScalarType &canonical_struct_scalar_type(const std::type_info &identity, std::string_view full_name,
                                                     std::string_view name, std::size_t size, std::size_t alignment,
                                                     std::span<const StructScalarField> fields) {
    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<std::type_index, const ResolvedStructScalarType *>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = types.find(key); entry != types.end()) {
        const auto &existing = *entry->second;
        const auto mismatch = [&] {
            throw std::logic_error{"conflicting native struct declaration: " + std::string{full_name}};
        };

        if (existing.full_name().text() != full_name || existing.size() != size || existing.alignment() != alignment ||
            existing.field_count() != fields.size()) {
            mismatch();
        }

        for (auto index = std::size_t{}; index < fields.size(); ++index) {
            if (existing.field(index).name != fields[index].name || existing.field(index).type != fields[index].type) {
                mismatch();
            }
        }

        return *entry->second;
    }

    const auto entry = types.try_emplace(key, nullptr).first;
    try {
        const auto &resolved = TypeRegistry::instance().register_owned(std::make_unique<ResolvedStructScalarType>(
            util::symbol{full_name}, util::symbol{name}, size, alignment,
            std::vector<StructScalarField>{fields.begin(), fields.end()}));
        entry->second = &resolved;
        return resolved;
    } catch (...) {
        types.erase(key);
        throw;
    }
}

} // namespace details

} // namespace bg
