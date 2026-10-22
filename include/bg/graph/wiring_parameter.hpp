#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/util/named_argument.hpp>
#include <bg/util/reflection.hpp>

#include <concepts>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

namespace details {

template <typename T>
inline constexpr bool wiring_parameter_default =
    supported_scalar_argument<T> || supported_optional_scalar_argument<T> ||
    supported_scalar_value_argument<T> || graph_scalar_view_argument<T>;

} // namespace details

class WiringParameter {
  public:
    template <util::compile_time_string Name>
    WiringParameter(details::compile_time_name<Name>) : name_{Name.view()} {}

    template <util::compile_time_string Name, typename Value>
        requires details::wiring_parameter_default<Value>
    WiringParameter(details::named_argument<Name, Value> &&value)
        : name_{Name.view()}, default_argument_{make_default(std::move(value.value))} {}

    template <util::compile_time_string Name, typename Value>
        requires details::wiring_parameter_default<Value>
    WiringParameter(const details::named_argument<Name, Value> &value)
        : name_{Name.view()}, default_argument_{make_default(value.value)} {}

    WiringParameter(std::string_view name) : name_{name} {}
    WiringParameter(const char *name) : name_{name} {}

    template <typename Value>
        requires details::wiring_parameter_default<Value>
    WiringParameter(std::string_view name, Value &&value)
        : name_{name}, default_argument_{make_default(std::forward<Value>(value))} {}

    util::symbol name() const noexcept { return name_; }
    const std::shared_ptr<const WiringArgument> &default_argument() const noexcept { return default_argument_; }

  private:
    template <typename Value>
        requires details::wiring_parameter_default<Value>
    std::shared_ptr<const WiringArgument> make_default(Value &&value) {
        auto argument = WiringArgument{name_, std::forward<Value>(value)};
        auto memory = allocator{};
        argument = argument.clone(memory);
        return std::make_shared<const WiringArgument>(std::move(argument));
    }

    util::symbol name_;
    std::shared_ptr<const WiringArgument> default_argument_;
};

namespace details {

template <typename Defaults, std::size_t... Index>
std::vector<WiringParameter> aggregate_wiring_parameters(const Defaults &defaults,
                                                         std::index_sequence<Index...>) {
    const auto members = util::aggregate_members(defaults);
    return {WiringParameter{util::aggregate_member_names<Defaults>[Index], std::get<Index>(members)}...};
}

template <typename Defaults>
std::vector<WiringParameter> aggregate_wiring_parameters(const Defaults &defaults) {
    return aggregate_wiring_parameters(
        defaults, std::make_index_sequence<util::aggregate_member_count<Defaults>>{});
}

} // namespace details

} // namespace bg
