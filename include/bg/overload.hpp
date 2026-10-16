#pragma once

#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/util/reflection.hpp>
#include <bg/util/symbol.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace bg {

template <auto &OP, typename Definition = void>
    requires std::derived_from<std::remove_cvref_t<decltype(OP)>, Operator>
class overload final {
  public:
    overload()
        requires requires { node<Definition>(); }
        : implementation_{&OP.overload(node<Definition>())} {}

    void set_name(util::symbol name) { implementation_->set_name(name); }

  private:
    WiringTemplate *implementation_;
};

template <auto &OP>
class overload<OP, void> final {
  public:
    template <typename Implementation>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate>
    explicit overload(Implementation &&implementation)
        : implementation_{&OP.overload(std::forward<Implementation>(implementation))} {}

    template <typename... Arguments>
        requires(sizeof...(Arguments) != 0 &&
                 (!std::derived_from<std::remove_cvref_t<Arguments>, WiringTemplate> && ...) &&
                 requires(Arguments &&...arguments) { node(std::forward<Arguments>(arguments)...); })
    explicit overload(Arguments &&...arguments)
        : implementation_{&OP.overload(node(std::forward<Arguments>(arguments)...))} {}

    void set_name(util::symbol name) { implementation_->set_name(name); }

  private:
    WiringTemplate *implementation_;
};

template <auto &OP, template <typename> typename DefinitionTemplate, typename... Types>
    requires std::derived_from<std::remove_cvref_t<decltype(OP)>, Operator> && (sizeof...(Types) != 0) &&
             (requires { node<DefinitionTemplate<Types>>(); } && ...)
class instantiate_overloads final {
  public:
    instantiate_overloads() : implementations_{&OP.overload(node<DefinitionTemplate<Types>>())...} {}

    void set_name(util::symbol name) {
        for (auto *implementation : implementations_) {
            implementation->set_name(name);
        }
    }

  private:
    std::array<WiringTemplate *, sizeof...(Types)> implementations_;
};

namespace details {

template <typename Registration>
concept overload_registration = requires(Registration &registration, util::symbol name) {
    registration.set_name(name);
};

template <typename Group, std::size_t... Index>
consteval bool overload_registration_group(std::index_sequence<Index...>) {
    return (overload_registration<util::aggregate_member_type<Index, Group>> && ...);
}

template <typename Group, std::size_t... Index>
void name_overloads(Group &group, std::index_sequence<Index...>) {
    auto members = util::aggregate_members(group);
    constexpr auto names = util::aggregate_member_names<Group>;
    (std::get<Index>(members).set_name(util::symbol{names[Index]}), ...);
}

} // namespace details

template <typename Group> void register_overloads() {
    static_assert(std::is_aggregate_v<Group>, "overload registration requires an aggregate group");

    constexpr auto Members = util::aggregate_member_count<Group>;
    static_assert(details::overload_registration_group<Group>(std::make_index_sequence<Members>{}),
                  "every overload registration group member must be an overload registration");

    auto registrations = Group{};
    details::name_overloads(registrations, std::make_index_sequence<Members>{});
}

} // namespace bg
