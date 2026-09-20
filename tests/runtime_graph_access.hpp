#pragma once

#include <bg/graph/runtime_graph.hpp>

#include <cstddef>

namespace bg::testing {

class RuntimeGraphAccess {
  public:
    static void schedule(RuntimeGraph &graph, std::size_t node_index) noexcept { graph.schedule(node_index); }

    static void set_owner(RuntimeGraph &graph, RuntimeGraph &owner_graph, std::size_t owner_node_index,
                          RuntimeGraphOwner owner) noexcept {
        graph.set_owner(owner_graph, owner_node_index, owner);
    }
};

} // namespace bg::testing
