#pragma once

#include <bg/export.hpp>
#include <bg/operator.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/details/typed_signature.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_template.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

BG_EXPORT std::optional<WiringPoint> wire_dispatch(GraphWiring &wiring, const Signature &dispatch_signature,
                                                    bool has_output, const TimeSeriesType *output_contract,
                                                    const Signature &prototype,
                                                    std::span<const WiringArgument> supplied,
                                                    std::span<const WiringTemplate *const> implementations,
                                                    const Operator &resolver, const TypeMap &seed_type_map,
                                                    std::span<const std::size_t> dispatch_inputs);

template <typename Prototype> class DispatchTemplate final : public WiringTemplate {
  public:
    DispatchTemplate() : signature_{make_signature()}, resolver_{signature_} {}

    template <typename... Implementations>
        requires(sizeof...(Implementations) > 0 &&
                 (std::derived_from<std::remove_cvref_t<Implementations>, WiringTemplate> && ...))
    explicit DispatchTemplate(Implementations &&...implementations) : DispatchTemplate{} {
        (overload(std::forward<Implementations>(implementations)), ...);
    }

    template <typename Implementation>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate>
    DispatchTemplate &overload(Implementation &&implementation) {
        using Stored = std::remove_cvref_t<Implementation>;
        implementations_.push_back(std::make_unique<Stored>(std::forward<Implementation>(implementation)));
        return *this;
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    auto operator()(Arguments &&...arguments) const & {
        auto values = details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
        return public_result(WiringTemplate::operator()(std::span<const WiringArgument>{values}));
    }

    auto operator()(std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(arguments));
    }

    auto operator()(std::span<const WiringArgument> arguments) const & {
        return public_result(WiringTemplate::operator()(arguments));
    }

    template <typename... Arguments>
        requires(details::graph_code_argument<Arguments> && ...)
    auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        auto values = details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
        return public_result(
            WiringTemplate::operator()(type_map, std::span<const WiringArgument>{values}));
    }

    auto operator()(const TypeMap &type_map, std::span<const WiringPoint> arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, arguments));
    }

    auto operator()(const TypeMap &type_map, std::span<const WiringArgument> arguments) const & {
        return public_result(WiringTemplate::operator()(type_map, arguments));
    }

    template <typename... Arguments> auto operator()(Arguments &&...) const && = delete;
    auto operator()(std::span<const WiringPoint>) const && = delete;
    auto operator()(std::span<const WiringArgument>) const && = delete;
    template <typename... Arguments> auto operator()(const TypeMap &, Arguments &&...) const && = delete;

  private:
    static auto public_result(std::optional<WiringPoint> result) {
        if constexpr (details::operator_typed_prototype<Prototype>::has_output) {
            return result ? wp<>{*result} : wp<>{};
        } else if (result) {
            throw std::logic_error{"an outputless dispatch produced an output"};
        }
    }

    static Signature make_signature() {
        auto names = std::vector<std::string>{};
        names.reserve(details::operator_typed_prototype<Prototype>::argument_count);
        for (auto index = std::size_t{}; index < details::operator_typed_prototype<Prototype>::argument_count; ++index) {
            names.push_back(std::to_string(index));
        }

        auto name_views = std::vector<std::string_view>{};
        name_views.reserve(names.size());
        for (const auto &name : names) {
            name_views.push_back(name);
        }
        return details::operator_typed_prototype<Prototype>::signature(util::symbol{"dispatch"}, name_views);
    }

    const Signature &default_signature() const final { return signature_; }

    std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                    const WiringArguments &argument_view,
                                    std::span<const WiringArgument> supplied) const final {
        auto implementations = std::vector<const WiringTemplate *>{};
        implementations.reserve(implementations_.size());
        for (const auto &implementation : implementations_) {
            implementations.push_back(implementation.get());
        }
        return wire_dispatch(wiring, signature_, signature_.has_output(), signature.output, signature_, supplied,
                             implementations, resolver_, argument_view.type_map(), {});
    }

    Signature signature_;
    Operator resolver_;
    std::vector<std::unique_ptr<WiringTemplate>> implementations_;
};

} // namespace bg::details

namespace bg {

template <typename Prototype> using dispatch_ = details::DispatchTemplate<Prototype>;

} // namespace bg
