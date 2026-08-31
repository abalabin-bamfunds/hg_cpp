#pragma once

#include <bg/graph/runtime_graph.hpp>

#include <cstddef>

namespace bg::testing {

class RuntimeGraphAccess {
  public:
    static void schedule(RuntimeGraph &graph, std::size_t node_index) noexcept { graph.schedule(node_index); }
};

} // namespace bg::testing
