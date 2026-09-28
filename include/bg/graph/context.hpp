#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/operators.hpp>
#include <bg/util/named_argument.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {
struct ResolvedSignature;
}

namespace bg::util {
class thread_local_pointer;
}

namespace bg::details {

class ContextScope {
  public:
    ContextScope(util::symbol name, WiringArgument argument);
    ~ContextScope();

    ContextScope(const ContextScope &) = delete;
    ContextScope &operator=(const ContextScope &) = delete;
    ContextScope(ContextScope &&) = delete;
    ContextScope &operator=(ContextScope &&) = delete;

    util::symbol name() const noexcept { return name_; }
    const WiringArgument &argument() const noexcept { return argument_; }

    static std::optional<WiringArgument> find(util::symbol name);
    static std::optional<WiringArgument> import(util::symbol name);

  private:
    static util::thread_local_pointer &active_scope();

    util::symbol name_;
    WiringArgument argument_;
    ContextScope *previous_;
};

class ContextScopes {
  public:
    static ContextScopes activate_context_parameters(const ResolvedSignature &signature,
                                                     std::span<const WiringArgument> arguments);

    ~ContextScopes();

    ContextScopes(const ContextScopes &) = delete;
    ContextScopes &operator=(const ContextScopes &) = delete;

  private:
    ContextScopes(const ResolvedSignature &signature, std::span<const WiringArgument> arguments);

    std::vector<std::unique_ptr<ContextScope>> scopes_;
};

template <typename Value> WiringArgument context_wiring_argument(Value &&value) {
    if constexpr (wiring_point_argument<Value>) {
        return WiringArgument{std::forward<Value>(value)};
    } else {
        return WiringArgument{operators::const_(std::forward<Value>(value))};
    }
}

} // namespace bg::details

namespace bg {

template <util::compile_time_string Name, typename Value>
[[nodiscard]] auto with_context(details::named_argument<Name, Value> binding) {
    return details::ContextScope{util::symbol{Name.view()}, details::context_wiring_argument(std::move(binding.value))};
}

template <util::compile_time_string Name, typename Value, typename Function>
decltype(auto) with_context(details::named_argument<Name, Value> binding, Function &&function) {
    auto scope = with_context(std::move(binding));
    return std::invoke(std::forward<Function>(function));
}

} // namespace bg
