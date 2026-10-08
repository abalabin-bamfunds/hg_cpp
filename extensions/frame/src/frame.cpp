#include <bg/frame/frame.hpp>
#include <bg/type/type_registry.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace bg::frame::details {
namespace {

class CanonicalFrameScalarType final : public ScalarType {
  public:
    explicit CanonicalFrameScalarType(std::string name) : name_{std::move(name)} {}

    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    const std::string &name() const noexcept { return name_; }

  private:
    std::string name_;
};

struct CanonicalFrameSeriesEntry {
    std::string name;
    const TimeSeriesType *type;
};

auto canonical_mutex = std::mutex{};
auto canonical_scalar_types = std::unordered_map<std::type_index, const CanonicalFrameScalarType *>{};
auto canonical_series_types = std::unordered_map<std::type_index, CanonicalFrameSeriesEntry>{};
auto canonical_series_runtimes = std::unordered_map<std::type_index, std::unique_ptr<TimeSeriesRuntimeType>>{};

} // namespace

const ScalarType &canonical_frame_scalar_type(const std::type_info &identity, std::string_view name) {
    const auto lock = std::lock_guard{canonical_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = canonical_scalar_types.find(key); entry != canonical_scalar_types.end()) {
        if (entry->second->name() != name)
            throw std::logic_error{"conflicting frame schema declaration: " + std::string{name}};
        return *entry->second;
    }

    const auto entry = canonical_scalar_types.try_emplace(key, nullptr).first;
    try {
        entry->second = &TypeRegistry::instance().register_owned(
            std::make_unique<CanonicalFrameScalarType>(std::string{name}));
        return *entry->second;
    } catch (...) {
        canonical_scalar_types.erase(key);
        throw;
    }
}

const TimeSeriesType &canonical_frame_series_type(const std::type_info &identity, std::string_view name,
                                                  TimeSeriesGuarantee guarantee,
                                                  frame_series_type_factory factory) {
    const auto lock = std::lock_guard{canonical_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = canonical_series_types.find(key); entry != canonical_series_types.end()) {
        const auto &type = *entry->second.type;
        if (entry->second.name != name || type.guarantee().value() != guarantee)
            throw std::logic_error{"conflicting frame time series declaration: " + std::string{name}};
        return type;
    }

    const auto entry = canonical_series_types
                           .try_emplace(key, CanonicalFrameSeriesEntry{std::string{name}, nullptr})
                           .first;
    try {
        entry->second.type = &TypeRegistry::instance().register_owned(factory(guarantee));
        return *entry->second.type;
    } catch (...) {
        canonical_series_types.erase(key);
        throw;
    }
}

const TimeSeriesRuntimeType &canonical_frame_series_runtime_type(const std::type_info &identity,
                                                                 const TimeSeriesType &type,
                                                                 frame_series_runtime_factory factory) {
    const auto lock = std::lock_guard{canonical_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = canonical_series_runtimes.find(key); entry != canonical_series_runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"frame runtime requested for a non-canonical schema type"};
        return *entry->second;
    }

    auto runtime = factory(type);
    const auto *result = runtime.get();
    canonical_series_runtimes.emplace(key, std::move(runtime));
    return *result;
}

} // namespace bg::frame::details
