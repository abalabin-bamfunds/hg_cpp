#pragma once

#include <bg/graph/wiring_point.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace bg {

class GraphWiring;
class TimeSeriesType;
namespace details {

// An owning, index-independent description of one isolated graph wiring.
class GraphWiringStructure {
  public:
    struct Data;

    GraphWiringStructure() = default;

    bool cacheable() const noexcept;
    std::size_t hash() const noexcept;
    std::size_t boundary_count() const noexcept;
    const TimeSeriesType &boundary_type(std::size_t index) const noexcept;
    const TimeSeriesType *result_type() const noexcept;

    friend bool operator==(const GraphWiringStructure &left, const GraphWiringStructure &right);

  private:
    explicit GraphWiringStructure(std::shared_ptr<const Data> data) : data_{std::move(data)} {}

    std::shared_ptr<const Data> data_;

    friend GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const WiringPoint>,
                                                       std::optional<WiringPoint>);
    friend GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const std::size_t>,
                                                       std::optional<std::size_t>);
};

// Boundary and runtime-input order are semantic. Wiring paths, labels, parameter names, and node indices are not.
GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring,
                                            std::span<const WiringPoint> boundaries,
                                            std::optional<WiringPoint> result = std::nullopt);

// Supports generated inner-graph nodes, which retain their boundary node indices.
GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring,
                                            std::span<const std::size_t> boundary_nodes,
                                            std::optional<std::size_t> result_node = std::nullopt);

} // namespace details
} // namespace bg
