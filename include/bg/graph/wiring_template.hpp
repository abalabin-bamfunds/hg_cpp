#pragma once

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/type/signature.hpp>

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class WiringTemplate {
  public:
    virtual ~WiringTemplate();

    virtual const Signature &signature() const = 0;

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] std::optional<WiringPoint> operator()(Arguments &&...arguments) const & {
        std::array<details::wiring_argument, sizeof...(Arguments)> values{
            details::wiring_argument{std::forward<Arguments>(arguments)}...};

        return WiringContext::current().wiring().invoke(*this, std::span<details::wiring_argument>{values});
    }

    [[nodiscard]] std::optional<WiringPoint> operator()(std::span<const WiringPoint> arguments) const & {
        auto values = std::vector<details::wiring_argument>{};
        values.reserve(arguments.size());

        for (const auto argument : arguments) {
            values.emplace_back(argument);
        }

        return WiringContext::current().wiring().invoke(*this, values);
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &bindings, Arguments &&...arguments) const & {
        std::array<details::wiring_argument, sizeof...(Arguments)> values{
            details::wiring_argument{std::forward<Arguments>(arguments)}...};

        return WiringContext::current().wiring().invoke(*this, std::span<details::wiring_argument>{values}, &bindings);
    }

    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &bindings,
                                                        std::span<const WiringPoint> arguments) const & {
        auto values = std::vector<details::wiring_argument>{};
        values.reserve(arguments.size());

        for (const auto argument : arguments) {
            values.emplace_back(argument);
        }

        return WiringContext::current().wiring().invoke(*this, values, &bindings);
    }

    template <typename... Arguments>
    [[nodiscard]] std::optional<WiringPoint> operator()(Arguments &&...) const && = delete;

    [[nodiscard]] std::optional<WiringPoint> operator()(std::span<const WiringPoint>) const && = delete;
    template <typename... Arguments>
    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &, Arguments &&...) const && = delete;
    [[nodiscard]] std::optional<WiringPoint> operator()(const TypeMap &,
                                                        std::span<const WiringPoint>) const && = delete;

  private:
    friend class GraphWiring;

    virtual util::symbol label(const ResolvedSignature &, std::span<details::wiring_argument>,
                                             util::symbol default_label) const {
        return default_label;
    }

    [[nodiscard]] virtual std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                          const TypeMap &bindings,
                                                          std::span<details::wiring_argument> arguments) const = 0;
};

} // namespace bg
