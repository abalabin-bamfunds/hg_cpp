#include <bg/graph/graph_argument.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/time_series_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg {
namespace {

class GraphArgumentBoundaryNode final : public NodeTemplate {
  public:
    explicit GraphArgumentBoundaryNode(const TimeSeriesType &type)
        : signature_{util::symbol{"graph_boundary"}, {}, type} {}

    const Signature &default_signature() const final { return signature_; }
    bool deduplicate_wiring() const noexcept final { return false; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept final { return nullptr; }

  private:
    Signature signature_;
};

struct GraphTypeKey {
    std::vector<const Type *> parameters;
    const TimeSeriesType *output;

    bool operator==(const GraphTypeKey &) const = default;
};

struct GraphTypeKeyHash {
    std::size_t operator()(const GraphTypeKey &key) const noexcept {
        auto result = std::hash<const TimeSeriesType *>{}(key.output);
        for (const auto *parameter : key.parameters) {
            result ^= std::hash<const Type *>{}(parameter) + (result << 6) + (result >> 2);
        }
        return result;
    }
};

} // namespace

GraphArgumentType::GraphArgumentType(std::vector<const Type *> parameters, const TimeSeriesType *output)
    : signature_{true}, parameters_{std::move(parameters)}, output_{output} {}

void GraphArgumentType::format_name(std::string &result) const {
    if (!has_signature()) {
        result += "GRAPH_T";
        return;
    }

    result += "GRAPH<auto(";
    for (auto index = std::size_t{}; index < parameters().size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        parameters()[index]->format_name(result);
    }
    result += ") -> ";
    if (output() == nullptr) {
        result += "void";
    } else {
        output()->format_name(result);
    }
    result += '>';
}

bool GraphArgumentType::is_resolved() const noexcept {
    return !signature_ || (std::ranges::all_of(parameters_, [](const auto *type) { return type->is_resolved(); }) &&
                           (output_ == nullptr || output_->is_resolved()));
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

bool GraphArgumentType::do_match(const Type &supplied, TypeMap &type_map) const {
    const auto *graph = type_cast<GraphArgumentType>(&supplied);
    if (graph == nullptr) {
        return false;
    }

    auto trial = type_map.overlay();
    if (!match_signature(*graph, trial)) {
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

    const auto *output = output_ == nullptr ? nullptr : type_cast<TimeSeriesType>(&output_->resolve(type_map));
    if (output_ != nullptr && output == nullptr) {
        throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, util::symbol{"GRAPH"},
                                  "graph output did not resolve to a time series type"};
    }

    return graph_argument_type(parameters, output);
}

const GraphArgumentType &graph_argument_type(std::span<const Type *const> parameters,
                                             const TimeSeriesType *output) {
    if (std::ranges::any_of(parameters, [](const auto *type) { return type == nullptr; })) {
        throw std::invalid_argument{"a graph type requires non-null parameter types"};
    }

    static auto mutex = std::mutex{};
    static auto types = std::unordered_map<GraphTypeKey, const GraphArgumentType *, GraphTypeKeyHash>{};

    auto key = GraphTypeKey{std::vector<const Type *>{parameters.begin(), parameters.end()}, output};
    const auto lock = std::lock_guard{mutex};
    auto entry = types.try_emplace(std::move(key), nullptr).first;
    if (entry->second == nullptr) {
        try {
            entry->second = &TypeRegistry::instance().register_owned(
                std::make_unique<GraphArgumentType>(entry->first.parameters, output));
        } catch (...) {
            types.erase(entry);
            throw;
        }
    }
    return *entry->second;
}

const GraphArgumentType &graph_argument_type(const details::GraphWiringStructure &structure) {
    auto parameters = std::vector<const Type *>{};
    parameters.reserve(structure.boundary_count());
    for (auto index = std::size_t{}; index < structure.boundary_count(); ++index) {
        parameters.push_back(&structure.boundary_type(index));
    }
    return graph_argument_type(parameters, structure.result_type());
}

GraphArgumentConstraint::GraphArgumentConstraint(details::GraphWiringStructure structure)
    : structures_{std::move(structure)} {}

GraphArgumentConstraint::GraphArgumentConstraint(std::vector<details::GraphWiringStructure> structures)
    : structures_{std::move(structures)} {
    if (std::get<std::vector<details::GraphWiringStructure>>(structures_).empty()) {
        structures_.emplace<std::monostate>();
    }
}

GraphArgumentConstraint::GraphArgumentConstraint(std::shared_ptr<const WiringTemplate> pattern,
                                                 const void *pattern_identity)
    : pattern_{std::move(pattern)}, pattern_identity_{pattern_identity == nullptr ? pattern_.get() : pattern_identity} {
    if (!pattern_) {
        throw std::invalid_argument{"a graph argument pattern constraint requires a pattern"};
    }
}

bool GraphArgumentConstraint::constrained() const noexcept {
    return !std::holds_alternative<std::monostate>(structures_) || pattern_ != nullptr;
}

bool GraphArgumentConstraint::same_constraint(const GraphArgumentConstraint &other) const {
    if (static_cast<bool>(pattern_) != static_cast<bool>(other.pattern_)) {
        return false;
    }
    if (pattern_) {
        return pattern_identity_ == other.pattern_identity_;
    }
    return structures_ == other.structures_;
}

std::partial_ordering GraphArgumentConstraint::compare_specificity(const GraphArgumentConstraint &other) const {
    if (constrained() != other.constrained()) {
        return constrained() ? std::partial_ordering::less : std::partial_ordering::greater;
    }
    if (!constrained() || same_constraint(other)) {
        return std::partial_ordering::equivalent;
    }
    return std::partial_ordering::unordered;
}

bool GraphArgumentConstraint::matches_pattern(const details::GraphWiringStructure &supplied) const {
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
        auto result = (*pattern_)(std::span<const WiringArgument>{arguments});
        if (result) {
            result = wiring.complete_pending_wiring(*result);
        } else {
            (void)wiring.complete_pending_wiring();
        }
        const auto structure = details::graph_wiring_structure(wiring, boundaries, result);
        wiring.finalize();
        return structure == supplied;
    } catch (const TypeResolutionError &) {
        return false;
    } catch (const WiringError &) {
        return false;
    }
}

bool GraphArgumentConstraint::matches(const GraphArgument &argument) const {
    if (!constrained()) {
        return true;
    }

    if (pattern_) {
        const auto *single = std::get_if<details::GraphWiringStructure>(&argument.structures());
        return single != nullptr && matches_pattern(*single);
    }

    if (const auto *single = std::get_if<details::GraphWiringStructure>(&structures_)) {
        const auto *supplied = std::get_if<details::GraphWiringStructure>(&argument.structures());
        return supplied != nullptr && *single == *supplied;
    }
    if (const auto *multiple = std::get_if<std::vector<details::GraphWiringStructure>>(&structures_)) {
        const auto *supplied = std::get_if<std::vector<details::GraphWiringStructure>>(&argument.structures());
        return supplied != nullptr && *multiple == *supplied;
    }
    return true;
}

const details::GraphWiringStructure &GraphArgument::structure() const {
    const auto *structure = std::get_if<details::GraphWiringStructure>(&structures_);
    if (structure == nullptr) {
        throw std::logic_error{"a graph argument does not contain one graph structure"};
    }
    return *structure;
}

GraphArgument::GraphArgument(details::GraphWiringStructure structure, std::shared_ptr<const void> payload,
                             const std::type_info *payload_type)
    : structures_{std::move(structure)},
      type_{&graph_argument_type(std::get<details::GraphWiringStructure>(structures_))},
      payload_{std::move(payload)},
      payload_type_{payload_type} {}

GraphArgument::GraphArgument(std::vector<details::GraphWiringStructure> structures,
                             std::shared_ptr<const void> payload, const std::type_info *payload_type)
    : structures_{std::move(structures)}, type_{&type()}, payload_{std::move(payload)}, payload_type_{payload_type} {}

const GraphArgumentType &GraphArgument::type() noexcept {
    static const auto type = GraphArgumentType{};
    [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type);
    return type;
}

} // namespace bg
