#include <bg/frame/arrow.hpp>

#include <mutex>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

namespace bg::frame::arrow_details {
namespace {

auto runtimes_mutex = std::mutex{};
auto runtimes = std::unordered_map<std::type_index, std::unique_ptr<ScalarRuntimeType>>{};

} // namespace

const ScalarRuntimeType &canonical_scalar_runtime(const ScalarType &type, const std::type_info &identity,
                                                  scalar_runtime_factory factory) {
    const auto lock = std::lock_guard{runtimes_mutex};
    const auto key = std::type_index{identity};
    if (const auto entry = runtimes.find(key); entry != runtimes.end()) {
        if (&entry->second->type() != &type)
            throw std::logic_error{"conflicting Arrow scalar runtime identity"};
        return *entry->second;
    }

    auto runtime = factory();
    if (&runtime->type() != &type)
        throw std::logic_error{"Arrow scalar runtime has a non-canonical scalar type"};
    auto [entry, inserted] = runtimes.emplace(key, std::move(runtime));
    if (!inserted)
        return *entry->second;
    try {
        register_scalar_runtime_type(*entry->second);
    } catch (...) {
        runtimes.erase(entry);
        throw;
    }
    return *entry->second;
}

} // namespace bg::frame::arrow_details
