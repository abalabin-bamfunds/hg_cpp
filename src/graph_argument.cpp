#include <bg/graph/graph_argument.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <memory>
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

bool GraphArgumentType::do_match(const Type &supplied, TypeMap &) const {
    const auto *graph = type_cast<GraphArgumentType>(&supplied);
    if (graph == nullptr) {
        return false;
    }
    if (pattern_) {
        return graph->structure_ && matches_pattern(*graph->structure_);
    }
    if (!structure_) {
        return true;
    }

    return graph->structure_ && *structure_ == *graph->structure_;
}

const GraphArgumentType &GraphArgument::type() noexcept {
    static const auto type = GraphArgumentType{};
    return type;
}

} // namespace bg
