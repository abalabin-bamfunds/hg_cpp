#include <bg/graph/graph_argument.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace bg {
namespace {

class GraphArgumentBoundaryNode final : public NodeTemplate {
  public:
    explicit GraphArgumentBoundaryNode(const TimeSeriesType &type)
        : signature_{util::symbol{"graph_boundary"}, {}, type} {}

    const Signature &signature() const final { return signature_; }
    bool deduplicate_wiring() const noexcept final { return false; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }

  private:
    Signature signature_;
};

} // namespace

GraphArgumentType::GraphArgumentType(details::GraphWiringStructure structure)
    : signature_{true}, structure_{std::move(structure)} {
    parameters_.reserve(structure_->boundary_count());
    for (auto index = std::size_t{}; index < structure_->boundary_count(); ++index) {
        parameters_.push_back(&structure_->boundary_type(index));
    }
    output_ = structure_->result_type();
}

GraphArgumentType::GraphArgumentType(std::shared_ptr<const WiringTemplate> pattern)
    : pattern_{std::move(pattern)}, pattern_identity_{pattern_.get()} {
    const auto &signature = pattern_->signature();
    signature_ = true;
    parameters_.reserve(signature.parameter_count());
    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        parameters_.push_back(signature.parameter(index).type);
    }
    output_ = signature.output();
}

GraphArgumentType::GraphArgumentType(std::vector<const Type *> parameters, const TimeSeriesType *output,
                                     std::shared_ptr<const WiringTemplate> pattern,
                                     const void *pattern_identity)
    : signature_{true}, parameters_{std::move(parameters)}, output_{output}, pattern_{std::move(pattern)},
      pattern_identity_{pattern_identity == nullptr ? pattern_.get() : pattern_identity} {}

bool GraphArgumentType::is_resolved() const noexcept {
    return !signature_ || (std::ranges::all_of(parameters_, [](const auto *type) { return type->is_resolved(); }) &&
                           (output_ == nullptr || output_->is_resolved()));
}

bool GraphArgumentType::same_constraint(const GraphArgumentType &other) const {
    if (static_cast<bool>(pattern_) != static_cast<bool>(other.pattern_) ||
        structure_.has_value() != other.structure_.has_value()) {
        return false;
    }
    if (pattern_) {
        return pattern_identity_ == other.pattern_identity_;
    }
    if (structure_) {
        return *structure_ == *other.structure_;
    }
    return true;
}

bool GraphArgumentType::match_signature(const GraphArgumentType &supplied, TypeMap &type_map) const {
    if (!signature_) {
        return true;
    }
    if (!supplied.signature_ || parameters_.size() != supplied.parameters_.size() ||
        (output_ == nullptr) != (supplied.output_ == nullptr)) {
        return false;
    }

    for (auto index = std::size_t{}; index < parameters_.size(); ++index) {
        if (!parameters_[index]->match(*supplied.parameters_[index], type_map)) {
            return false;
        }
    }
    return output_ == nullptr || output_->match(*supplied.output_, type_map);
}

bool GraphArgumentType::accepts_declaration(const GraphArgumentType &supplied) const {
    auto type_map = TypeMap{};
    return match_signature(supplied, type_map) && same_constraint(supplied);
}

bool GraphArgumentType::matches_pattern(const details::GraphWiringStructure &supplied) const {
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto boundaries = std::vector<WiringPoint>{};
    auto arguments = std::vector<WiringArgument>{};
    boundaries.reserve(supplied.boundary_count());
    arguments.reserve(supplied.boundary_count());

    for (auto index = std::size_t{}; index < supplied.boundary_count(); ++index) {
        auto boundary = std::make_unique<GraphArgumentBoundaryNode>(supplied.boundary_type(index));
        const auto &owned = wiring.own_node_template(std::move(boundary));
        const auto point = *owned(std::span<const WiringArgument>{});
        boundaries.push_back(point);
        arguments.emplace_back(point);
    }

    try {
        const auto result = (*pattern_)(std::span<const WiringArgument>{arguments});
        const auto structure = details::graph_wiring_structure(wiring, boundaries, result);
        return structure == supplied;
    } catch (const TypeResolutionError &) {
        return false;
    } catch (const WiringError &) {
        return false;
    }
}

bool GraphArgumentType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *graph = type_cast<GraphArgumentType>(&supplied);
    if (graph == nullptr) {
        return false;
    }

    auto trial = type_map.overlay();
    if (!match_signature(*graph, trial)) {
        return false;
    }
    if (pattern_) {
        if (!graph->structure_ || !matches_pattern(*graph->structure_)) {
            return false;
        }
    } else if (structure_ && (!graph->structure_ || *structure_ != *graph->structure_)) {
        return false;
    }

    type_map.apply(std::move(trial));
    return true;
}

const Type &GraphArgumentType::resolve(const TypeMap &type_map) const {
    if (is_resolved()) {
        return *this;
    }

    auto parameters = std::vector<const Type *>{};
    parameters.reserve(parameters_.size());
    for (const auto *parameter : parameters_) {
        parameters.push_back(&parameter->resolve(type_map));
    }

    const auto *output = output_ == nullptr
                             ? nullptr
                             : type_cast<TimeSeriesType>(&output_->resolve(type_map));
    if (output_ != nullptr && output == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"GRAPH"},
                                  "graph output did not resolve to a time series type"};
    }

    return graph_argument_type(parameters, output, pattern_, pattern_identity_);
}

namespace {

struct GraphTypeKey {
    std::vector<const Type *> parameters;
    const TimeSeriesType *output;
    const void *pattern_identity;

    bool operator==(const GraphTypeKey &) const = default;
};

struct GraphTypeKeyHash {
    std::size_t operator()(const GraphTypeKey &key) const noexcept {
        auto result = std::hash<const TimeSeriesType *>{}(key.output);
        result ^= std::hash<const void *>{}(key.pattern_identity) + (result << 6) + (result >> 2);
        for (const auto *parameter : key.parameters) {
            result ^= std::hash<const Type *>{}(parameter) + (result << 6) + (result >> 2);
        }
        return result;
    }
};

} // namespace

const GraphArgumentType &graph_argument_type(std::span<const Type *const> parameters,
                                             const TimeSeriesType *output,
                                             std::shared_ptr<const WiringTemplate> pattern,
                                             const void *pattern_identity) {
    if (std::ranges::any_of(parameters, [](const auto *type) { return type == nullptr || !type->is_resolved(); }) ||
        (output != nullptr && !output->is_resolved())) {
        throw std::invalid_argument{"a resolved graph type requires resolved parameter and output types"};
    }

    static auto mutex = std::mutex{};
    static auto types =
        std::unordered_map<GraphTypeKey, std::unique_ptr<GraphArgumentType>, GraphTypeKeyHash>{};

    if (pattern && pattern_identity == nullptr) {
        pattern_identity = pattern.get();
    }

    auto key = GraphTypeKey{
        std::vector<const Type *>{parameters.begin(), parameters.end()}, output, pattern_identity};
    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = types.try_emplace(std::move(key));
    if (inserted) {
        entry->second = std::make_unique<GraphArgumentType>(
            entry->first.parameters, output, std::move(pattern), pattern_identity);
    }
    return *entry->second;
}

const GraphArgumentType &GraphArgument::type() noexcept {
    static const auto type = GraphArgumentType{};
    return type;
}

} // namespace bg
