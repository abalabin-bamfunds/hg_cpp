#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/type/scalar_type.hpp>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace bg::details {

struct DispatchSelectorCandidate {
    std::vector<const ScalarType *> input_types;
};

struct DispatchSelectorData {
    std::pmr::vector<const ScalarType *> input_types;
    std::size_t input_count{};
};

template <typename CandidateType, typename InputType>
std::size_t select_dispatch_candidate(std::size_t candidate_count, std::size_t input_count,
                                      CandidateType &&candidate_type, InputType &&input_type) {
    auto inline_applicable = std::array<unsigned char, 8>{};
    auto extended_applicable = candidate_count > inline_applicable.size()
                                   ? std::vector<unsigned char>(candidate_count)
                                   : std::vector<unsigned char>{};
    auto applicable = candidate_count > inline_applicable.size()
                          ? std::span<unsigned char>{extended_applicable}
                          : std::span<unsigned char>{inline_applicable}.first(candidate_count);
    for (auto candidate = std::size_t{}; candidate < candidate_count; ++candidate) {
        applicable[candidate] = 1;
        for (auto input = std::size_t{}; input < input_count; ++input) {
            if (!candidate_type(candidate, input).accepts(input_type(input))) {
                applicable[candidate] = 0;
                break;
            }
        }
    }

    auto selected = std::optional<std::size_t>{};
    for (auto candidate = std::size_t{}; candidate < candidate_count; ++candidate) {
        if (!applicable[candidate]) {
            continue;
        }

        auto dominated = false;
        for (auto other = std::size_t{}; other < candidate_count; ++other) {
            if (candidate == other || !applicable[other]) {
                continue;
            }

            auto more_specific = true;
            auto strictly_more_specific = false;
            for (auto input = std::size_t{}; input < input_count; ++input) {
                const auto &candidate_input = candidate_type(candidate, input);
                const auto &other_input = candidate_type(other, input);
                if (!candidate_input.accepts(other_input)) {
                    more_specific = false;
                    break;
                }
                strictly_more_specific |= !other_input.accepts(candidate_input);
            }
            if (more_specific && strictly_more_specific) {
                dominated = true;
                break;
            }
        }
        if (dominated) {
            continue;
        }
        if (selected) {
            throw std::logic_error{"dispatch input types match ambiguous implementations"};
        }
        selected = candidate;
    }

    if (!selected) {
        throw std::out_of_range{"dispatch input types do not match an implementation"};
    }
    return *selected;
}

class DispatchSelectorDefinition {
  public:
    using compiled_data_type = DispatchSelectorData;

    DispatchSelectorDefinition(std::span<const WiringArgument> inputs,
                               std::span<const DispatchSelectorCandidate> candidates);

    const Signature &signature() const noexcept { return signature_; }

    DispatchSelectorData compile(const ResolvedSignature &, allocator &memory) const;

    template <typename Invocation> static void evaluate(Invocation invocation) {
        auto inline_types = std::array<const ScalarType *, 8>{};
        auto extended_types = invocation.parameter_count() > inline_types.size()
                                  ? std::vector<const ScalarType *>(invocation.parameter_count())
                                  : std::vector<const ScalarType *>{};
        auto input_types = invocation.parameter_count() > inline_types.size()
                               ? std::span<const ScalarType *>{extended_types}
                               : std::span<const ScalarType *>{inline_types}.first(invocation.parameter_count());
        for (auto index = std::size_t{}; index < invocation.parameter_count(); ++index) {
            const auto input = invocation.template input<In<TS<SCALAR>, optional>>(index);
            if (!input.valid()) {
                invocation.template output<TS<i64, TimeSeriesGuarantee::deduplicated>>().invalidate();
                return;
            }
            input_types[index] = &input.value_view().concrete_type();
        }

        const auto &compiled = invocation.template compiled_data<DispatchSelectorData>();
        const auto candidate_count = compiled.input_types.size() / compiled.input_count;
        const auto selected = select_dispatch_candidate(
            candidate_count, compiled.input_count,
            [&](std::size_t candidate, std::size_t input) -> const ScalarType & {
                return *compiled.input_types[candidate * compiled.input_count + input];
            },
            [&](std::size_t input) -> const ScalarType & { return *input_types[input]; });
        invocation.template output<TS<i64, TimeSeriesGuarantee::deduplicated>>().set(
            static_cast<i64>(selected));
    }

  private:
    std::vector<DispatchSelectorCandidate> candidates_;
    Signature signature_;
};

std::unique_ptr<NodeTemplate> make_dispatch_selector(std::span<const WiringArgument> inputs,
                                                      std::span<const DispatchSelectorCandidate> candidates);

} // namespace bg::details
