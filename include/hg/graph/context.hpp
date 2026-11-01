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

struct ContextBinding {
    util::symbol name;
    WiringArgument argument;
    const ContextBinding *previous{};
};

// Installs a borrowed lexical binding chain and restores the previous chain when its scope ends.
class ContextBindingState {
  public:
    explicit ContextBindingState(ContextBinding &binding) noexcept;
    explicit ContextBindingState(const ContextBinding *head) noexcept;
    ~ContextBindingState();

    ContextBindingState(const ContextBindingState &) = delete;
    ContextBindingState &operator=(const ContextBindingState &) = delete;
    ContextBindingState(ContextBindingState &&) = delete;
    ContextBindingState &operator=(ContextBindingState &&) = delete;

    static const ContextBinding *current() noexcept;
    static std::optional<WiringArgument> find(util::symbol name);

  private:
    static util::thread_local_pointer &active_binding();

    const ContextBinding *previous_;
};

class ContextScope {
  public:
    ContextScope(util::symbol name, WiringArgument argument);
    ~ContextScope();

    ContextScope(const ContextScope &) = delete;
    ContextScope &operator=(const ContextScope &) = delete;
    ContextScope(ContextScope &&) = delete;
    ContextScope &operator=(ContextScope &&) = delete;

    util::symbol name() const noexcept { return binding_.name; }
    const WiringArgument &argument() const noexcept { return binding_.argument; }

    static std::optional<WiringArgument> find(util::symbol name);
    static std::optional<WiringArgument> import(util::symbol name);

  private:
    ContextBinding binding_;
    ContextBindingState state_;
};

// Engine-facing API for retaining visible lexical bindings beyond their stack scopes.
class ContextSnapshot {
    struct Storage;

  public:
    class Activation {
      public:
        explicit Activation(const ContextSnapshot &snapshot) noexcept;
        ~Activation();

        Activation(const Activation &) = delete;
        Activation &operator=(const Activation &) = delete;
        Activation(Activation &&) = delete;
        Activation &operator=(Activation &&) = delete;

      private:
        std::shared_ptr<const Storage> storage_;
        ContextBindingState state_;
    };

    ContextSnapshot();
    // Captures visible bindings. The snapshot owns scalar values; time-series bindings retain their graph-owned points.
    static ContextSnapshot capture();

    // Temporarily makes only the saved bindings visible. The activation owns the snapshot storage until it restores
    // the prior context.
    [[nodiscard]] Activation activate() const noexcept { return Activation{*this}; }

  private:
    std::shared_ptr<const Storage> storage_;

    explicit ContextSnapshot(std::shared_ptr<const Storage> storage) noexcept;
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
