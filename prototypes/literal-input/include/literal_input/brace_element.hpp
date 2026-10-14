#pragma once

#include <concepts>
#include <functional>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace literal_input {

template <typename Leaf> class BraceElement {
  public:
    using sequence_type = std::span<const BraceElement>;

    BraceElement() noexcept = default;

    BraceElement(std::initializer_list<BraceElement> elements) noexcept
        : value_{std::in_place_index<1>, elements.begin(), elements.size()} {}

    template <typename Source>
        requires std::constructible_from<Leaf, Source &&> &&
                 (!std::same_as<std::remove_cvref_t<Source>, BraceElement>)
    BraceElement(Source &&source) : value_{std::in_place_index<0>, std::forward<Source>(source)} {}

    template <typename LeafVisitor, typename SequenceVisitor>
    void visit(LeafVisitor &&visit_leaf, SequenceVisitor &&visit_sequence) const {
        std::visit(
            [&](const auto &value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<Value, Leaf>) {
                    std::invoke(visit_leaf, value);
                } else {
                    std::invoke(visit_sequence, value);
                }
            },
            value_);
    }

  private:
    std::variant<Leaf, sequence_type> value_{std::in_place_index<1>};
};

} // namespace literal_input
