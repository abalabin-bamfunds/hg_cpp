#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/hash_dictionary.hpp>

#include <concepts>
#include <utility>

namespace bg {

// Stores one optional-operation handler per exact scalar type or runtime type. find(runtime_type) prefers the exact
// runtime handler, then falls back to its semantic type. Consumers that choose optional actions independently use
// find_exact() on each record. The registry does not modify type objects. Returned pointers remain stable until
// registry destruction.
template <typename Handler> class ScalarDispatch {
  public:
    // Defined only for operation registries whose owning library exports a shared anchor.
    static ScalarDispatch &instance() noexcept;

    ScalarDispatch() = default;
    explicit ScalarDispatch(allocator memory) noexcept : memory_{memory} {}

    ~ScalarDispatch() {
        runtime_handlers_.release(memory_.memory_resource());
        type_handlers_.release(memory_.memory_resource());
    }

    ScalarDispatch(const ScalarDispatch &) = delete;
    ScalarDispatch &operator=(const ScalarDispatch &) = delete;
    ScalarDispatch(ScalarDispatch &&) = delete;
    ScalarDispatch &operator=(ScalarDispatch &&) = delete;

    template <typename Source>
        requires std::constructible_from<Handler, Source>
    [[nodiscard]] bool add(const ScalarRuntimeType &runtime_type, Source &&handler) {
        return runtime_handlers_
            .try_emplace(&runtime_type, memory_.memory_resource(), std::forward<Source>(handler))
            .second;
    }

    template <typename Source>
        requires std::constructible_from<Handler, Source>
    [[nodiscard]] bool add(const ScalarType &type, Source &&handler) {
        return type_handlers_.try_emplace(&type, memory_.memory_resource(), std::forward<Source>(handler)).second;
    }

    const Handler *find(const ScalarRuntimeType &runtime_type) const noexcept {
        if (const auto *handler = find_exact(runtime_type)) {
            return handler;
        }
        return find_exact(runtime_type.type());
    }

    const Handler *find_exact(const ScalarRuntimeType &runtime_type) const noexcept {
        return runtime_handlers_.find(&runtime_type);
    }

    const Handler *find_exact(const ScalarType &type) const noexcept { return type_handlers_.find(&type); }

    const Handler *find(const ScalarType &type) const noexcept { return find_exact(type); }

  private:
    allocator memory_;
    util::hash_dictionary<const ScalarRuntimeType *, Handler> runtime_handlers_;
    util::hash_dictionary<const ScalarType *, Handler> type_handlers_;
};

} // namespace bg
