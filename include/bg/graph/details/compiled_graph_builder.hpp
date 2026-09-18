#pragma once

#include <bg/allocator.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace bg {

class CompiledGraph;
class GraphWiring;

namespace details {

// Compiles a graph and remaps the supplied logical node indices to physical node indices.
std::shared_ptr<const CompiledGraph> compile_graph(const GraphWiring &wiring, allocator memory,
                                                   std::span<std::size_t> node_indices);

} // namespace details
} // namespace bg
