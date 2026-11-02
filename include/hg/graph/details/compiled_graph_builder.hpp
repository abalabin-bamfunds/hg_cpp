#pragma once

#include <hg/allocator.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace hg {

class CompiledGraph;
class GraphWiring;

namespace details {

// Compiles a graph and replaces each supplied wired-node index with its compiled-node index.
std::shared_ptr<const CompiledGraph> compile_graph(const GraphWiring &wiring, allocator memory,
                                                   std::span<std::size_t> node_indices);

} // namespace details
} // namespace hg
