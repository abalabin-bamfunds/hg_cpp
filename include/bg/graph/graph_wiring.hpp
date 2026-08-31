#pragma once

#include <bg/graph/details/wiring_argument.hpp>
#include <bg/graph/wired_node.hpp>
#include <bg/graph/wiring_path.hpp>
#include <bg/graph/wiring_point.hpp>

#include <array>
#include <deque>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class NodeTemplate;
class TimeSeriesType;
class WiringTemplate;

struct WiringPointRecord {
    static constexpr auto npos = static_cast<std::size_t>(-1);

    std::size_t node_index;
    std::size_t parent;
    std::size_t item;
    const TimeSeriesType *type;
};

class GraphWiring {
  public:
    GraphWiring();
    explicit GraphWiring(std::pmr::memory_resource &memory);
    ~GraphWiring();

    GraphWiring(const GraphWiring &) = delete;
    GraphWiring &operator=(const GraphWiring &) = delete;
    GraphWiring(GraphWiring &&) = delete;
    GraphWiring &operator=(GraphWiring &&) = delete;

    [[nodiscard]] std::span<const WiredNode> nodes() const noexcept { return nodes_; }

    [[nodiscard]] std::span<const WiringPointRecord> wiring_points() const noexcept { return wiring_points_; }

  private:
    friend class NodeTemplate;
    friend class WiringPoint;
    friend class WiringTemplate;

    [[nodiscard]] std::optional<WiringPoint> invoke(const WiringTemplate &wiring_template,
                                                    std::span<details::wiring_argument> arguments);
    [[nodiscard]] std::optional<WiringPoint> add_node(const NodeTemplate &node_template, ResolvedSignature signature,
                                                      std::vector<const ScalarType *> state_types,
                                                      std::span<details::wiring_argument> arguments);
    [[nodiscard]] WiringPoint select_item(WiringPoint source, std::size_t index);
    [[nodiscard]] WiringPoint make_reference(WiringPoint source, const TimeSeriesType &reference_type);

    [[nodiscard]] const WiringPath &make_path(util::symbol name, util::symbol label);
    [[nodiscard]] util::symbol default_label() const;

    void destroy_scalars(std::span<const WiredScalar> scalars) noexcept;
    [[nodiscard]] std::pmr::memory_resource &variable_memory() noexcept;
    [[nodiscard]] std::pmr::memory_resource &variable_memory(const ScalarStorageRepresentation &representation);

    std::pmr::memory_resource *memory_;
    std::pmr::unsynchronized_pool_resource fixed_memory_;
    std::optional<std::pmr::unsynchronized_pool_resource> variable_memory_;
    std::vector<WiredNode> nodes_;
    std::vector<WiringPointRecord> wiring_points_;
    struct ReferenceWiring {
        std::size_t source;
        const TimeSeriesType *type;
        std::size_t reference;
    };
    std::vector<ReferenceWiring> references_;
    std::pmr::deque<WiringPath> paths_;
};

} // namespace bg
