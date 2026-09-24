# Mesh

## Purpose

`mesh(graph, arguments...)` runs one inner runtime graph for each selected key.
It uses the same argument, key-set, key-argument, output, and overload rules as
[`map`](map.md). A branch can also read another branch output by key:

```cpp
auto result = mesh(
    [](auto value, auto dependency_key) {
        return combine(value, mesh()[dependency_key]);
    },
    values, dependencies);
```

`mesh()[key]` is available only while the mesh graph is being wired. The
receiving input determines its time series type. The type must accept the mesh
graph output.

A mesh can have a name:

```cpp
auto result = mesh("name"_n = "prices", graph, arguments...);
auto selected = mesh("prices")[key];
```

`mesh()[key]` selects from the nearest enclosing mesh. `mesh("name")[key]`
selects from the named enclosing mesh. Names must be unique among active
enclosing mesh scopes. Name resolution happens during wiring; runtime selection
does not compare names.

## Dependencies

The key can be a scalar or a scalar-value time series. An invalid time series
key clears that dependency. A changed key replaces the previous dependency.

Depending on a missing key creates its branch. A branch remains present while
its key is in the selected key set or another live branch depends on it. A
branch stops after both conditions become false. Several dependencies on one
branch share its runtime graph and output.

Dependencies persist across outer cycles. A clean dependency branch is not
evaluated again. When a dependency branch has work in the current cycle, mesh
evaluates it before resuming the branch that depends on it. This ordering is
iterative and each branch evaluates at most once in the outer cycle.

A dependency can occur inside any number of ordinary nested graphs, maps,
switches, and meshes. Each runtime dependency-node instance owns one dependency
edge. Instances of one compiled dependency node therefore depend independently.

A branch cannot select itself, directly or indirectly. Mesh reports the cycle
as a runtime error.

## Lifecycle and scheduling

Mesh compiles the graph once and creates one runtime instance per live key.
Immediate and timed scheduling follow the [nested graph rules](nested-graphs.md#evaluation).

Stopping branches release their dependencies before the dependency branches
stop. Destruction removes subscribers before destroying dependency outputs.
This preserves reference lifetime across the stopping cycle.
