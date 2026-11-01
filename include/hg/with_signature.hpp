#pragma once

#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_template.hpp>

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {

template <typename Target> class SignatureWiringTemplate final : public WiringTemplate {
  public:
    SignatureWiringTemplate(Target target, Signature signature)
        : target_{std::move(target)}, signature_{std::move(signature)} {}

    using WiringTemplate::operator();

    template <typename... Arguments>
        requires(graph_code_argument<Arguments> && ...) &&
                requires(const Target &target, Arguments &&...arguments) {
                    target(std::forward<Arguments>(arguments)...);
                }
    auto operator()(Arguments &&...arguments) const & {
        using Result = decltype(target_(std::forward<Arguments>(arguments)...));
        auto output = WiringTemplate::operator()(std::forward<Arguments>(arguments)...);
        if constexpr (std::same_as<Result, void>) {
            return;
        } else if constexpr (is_typed_wiring_point<Result>) {
            return Result{*output};
        } else {
            return output;
        }
    }

    template <typename... Arguments>
        requires(graph_code_argument<Arguments> && ...) &&
                requires(const Target &target, const TypeMap &type_map, Arguments &&...arguments) {
                    target(type_map, std::forward<Arguments>(arguments)...);
                }
    auto operator()(const TypeMap &type_map, Arguments &&...arguments) const & {
        using Result = decltype(target_(type_map, std::forward<Arguments>(arguments)...));
        auto output = WiringTemplate::operator()(type_map, std::forward<Arguments>(arguments)...);
        if constexpr (std::same_as<Result, void>) {
            return;
        } else if constexpr (is_typed_wiring_point<Result>) {
            return Result{*output};
        } else {
            return output;
        }
    }

    const Signature &default_signature() const override { return signature_; }
    bool allows_output_inference() const noexcept override { return target_.allows_output_inference(); }

  private:
    std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature, const WiringArguments &call,
                                    std::span<const WiringArgument> arguments) const override {
        const auto public_fixed_count = signature_.first_variadic_parameter().value_or(signature_.parameter_count());
        const auto target_fixed_count =
            target_.signature().first_variadic_parameter().value_or(target_.signature().parameter_count());
        const auto target_accepts_named = target_.signature().variadic_parameter(VariadicMode::named).has_value();

        auto forwarded = std::vector<WiringArgument>{};
        forwarded.reserve(arguments.size());
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            auto argument = WiringArgument::borrow(arguments[index]);
            if (target_accepts_named && index >= target_fixed_count && index < public_fixed_count) {
                argument.set_variadic_name(signature_.parameter(index).name, VariadicMode::named);
            }
            forwarded.push_back(std::move(argument));
        }

        return wiring.invoke(target_, forwarded, &call.type_map());
    }

    Target target_;
    Signature signature_;
};

} // namespace bg::details

namespace bg {

template <typename Target>
    requires std::derived_from<std::remove_cvref_t<Target>, WiringTemplate>
[[nodiscard]] auto with_signature(Target &&target, Signature signature) {
    return details::SignatureWiringTemplate<std::remove_cvref_t<Target>>{
        std::forward<Target>(target), std::move(signature)};
}

} // namespace bg
