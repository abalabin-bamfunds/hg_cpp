#include <bg/dispatch/details/dispatch_selector.hpp>

#include <bg/graph/node.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <stdexcept>

namespace bg::details {

DispatchSelectorDefinition::DispatchSelectorDefinition(
    std::span<const WiringArgument> inputs, std::span<const DispatchSelectorCandidate> candidates)
    : candidates_{candidates.begin(), candidates.end()}, signature_{[&] {
          auto parameters = std::vector<Parameter>{};
          parameters.reserve(inputs.size());
          for (auto index = std::size_t{}; index < inputs.size(); ++index) {
              const auto *time_series = type_cast<TimeSeriesType>(&inputs[index].type());
              const auto *value_type = time_series == nullptr
                                           ? nullptr
                                           : type_cast<TimeSeriesValueType>(&time_series->dereferenced_type());
              if (value_type == nullptr) {
                  throw std::invalid_argument{"dispatch selector inputs must be value time series"};
              }
              parameters.push_back({util::symbol{std::to_string(index)}, value_type, active, optional});
          }
          return Signature{util::symbol{"dispatch_selector"}, std::move(parameters),
                           TS<i64, TimeSeriesGuarantee::deduplicated>::type()};
      }()} {
    if (candidates_.empty()) {
        throw std::invalid_argument{"dispatch selector requires at least one candidate"};
    }
    if (inputs.empty()) {
        throw std::invalid_argument{"dispatch selector requires at least one input"};
    }
    for (const auto &candidate : candidates_) {
        if (candidate.input_types.size() != inputs.size()) {
            throw std::invalid_argument{"dispatch selector candidate has the wrong input count"};
        }
    }
}

DispatchSelectorData DispatchSelectorDefinition::compile(const ResolvedSignature &, allocator &memory) const {
    auto result = DispatchSelectorData{.input_types = std::pmr::vector<const ScalarType *>{&memory.memory_resource()},
                                       .input_count = candidates_.front().input_types.size()};
    for (const auto &candidate : candidates_) {
        result.input_types.insert(result.input_types.end(), candidate.input_types.begin(), candidate.input_types.end());
    }
    return result;
}

std::unique_ptr<NodeTemplate> make_dispatch_selector(std::span<const WiringArgument> inputs,
                                                      std::span<const DispatchSelectorCandidate> candidates) {
    return std::make_unique<RuntimeSignatureNodeTemplate<DispatchSelectorDefinition>>(
        DispatchSelectorDefinition{inputs, candidates});
}

} // namespace bg::details
