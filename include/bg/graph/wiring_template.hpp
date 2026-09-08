#pragma once

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/requirement.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/type/signature.hpp>

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class WiringArguments;

class WiringTemplate {
  public:
    virtual ~WiringTemplate();

    virtual const Signature &signature() const = 0;
    [[nodiscard]] virtual RequirementResult requirement(const WiringArguments &) const { return accept; }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    std::optional<WiringPoint> operator()(Arguments &&...arguments) const & {
        std::array<details::wiring_argument, sizeof...(Arguments)> values{
            details::wiring_argument{std::forward<Arguments>(arguments)}...};

        return WiringContext::current().wiring().invoke(*this, std::span<details::wiring_argument>{values});
    }

    std::optional<WiringPoint> operator()(std::span<const WiringPoint> arguments) const & {
        auto values = std::vector<details::wiring_argument>{};
        values.reserve(arguments.size());

        for (const auto argument : arguments) {
            values.emplace_back(argument);
        }

        return WiringContext::current().wiring().invoke(*this, values);
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    std::optional<WiringPoint> operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        std::array<details::wiring_argument, sizeof...(Arguments)> values{
            details::wiring_argument{std::forward<Arguments>(arguments)}...};

        return WiringContext::current().wiring().invoke(*this, std::span<details::wiring_argument>{values}, &type_map);
    }

    std::optional<WiringPoint> operator()(const TypeMap &type_map, std::span<const WiringPoint> arguments) const & {
        auto values = std::vector<details::wiring_argument>{};
        values.reserve(arguments.size());

        for (const auto argument : arguments) {
            values.emplace_back(argument);
        }

        return WiringContext::current().wiring().invoke(*this, values, &type_map);
    }

    template <typename... Arguments> std::optional<WiringPoint> operator()(Arguments &&...) const && = delete;

    std::optional<WiringPoint> operator()(std::span<const WiringPoint>) const && = delete;
    template <typename... Arguments>
    std::optional<WiringPoint> operator()(const TypeMap &, Arguments &&...) const && = delete;
    std::optional<WiringPoint> operator()(const TypeMap &, std::span<const WiringPoint>) const && = delete;

  protected:
    [[nodiscard]] std::optional<WiringPoint> invoke_resolved(GraphWiring &wiring, SignatureResolution resolution,
                                                             std::span<details::wiring_argument> arguments,
                                                             util::symbol path_name) const;
    [[nodiscard]] std::optional<WiringPoint> invoke_resolved(GraphWiring &wiring, const WiringTemplate &implementation,
                                                             SignatureResolution resolution,
                                                             std::span<details::wiring_argument> arguments,
                                                             util::symbol path_name) const;

  private:
    friend class GraphWiring;

    [[nodiscard]] virtual std::optional<WiringPoint>
    invoke(GraphWiring &wiring, std::span<details::wiring_argument> arguments, const TypeMap *seed_type_map) const;

    virtual util::symbol label(const ResolvedSignature &, std::span<details::wiring_argument>,
                               util::symbol default_label) const {
        return default_label;
    }

    [[nodiscard]] virtual std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                          const TypeMap &type_map,
                                                          std::span<details::wiring_argument> arguments) const = 0;
};

} // namespace bg
