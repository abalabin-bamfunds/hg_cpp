# BG documentation

BG connects calculations whose inputs change over time. Connections define how
outputs depend on inputs. BG evaluates the calculations as updates arrive.

[Introduction to BG](guide/index.md) explains the concepts. [Installation](guide/installation.md)
and [A first BG program](guide/getting-started.md) provide a runnable starting point.

The guide progresses from expressions and functions to updates, custom nodes,
execution, collections, and advanced graph composition. [Task guides](how-to/choose-operation.md)
answer specific questions. The [API reference](reference/index.md) and
[library index](reference/library-index.md) specify contracts and supported types.

```{toctree}
:caption: Getting started
:maxdepth: 1

guide/index
guide/installation
guide/getting-started
```

```{toctree}
:caption: Graph basics
:maxdepth: 1

guide/arithmetic
guide/connect-calculations
guide/changing-values
guide/conditions
```

```{toctree}
:caption: Working with updates
:maxdepth: 1

guide/dedup
guide/filter
guide/sample
guide/count-and-lag
guide/running-calculations
```

```{toctree}
:caption: Writing nodes
:maxdepth: 1

guide/calculation-nodes
guide/skip-output
guide/optional-inputs
guide/validity
guide/input-views
guide/view-validity
```

```{toctree}
:caption: Running graphs
:maxdepth: 1

guide/run-graphs
guide/time
guide/schedule
guide/delay
guide/throttle
guide/reset-history
guide/state
guide/lifecycle
guide/sources
guide/queues
```

```{toctree}
:caption: Collections
:maxdepth: 1

guide/fixed-lists
guide/bundles
guide/dictionaries
guide/sets
guide/collection-reductions
guide/map
guide/reduce
```

```{toctree}
:caption: Advanced graphs
:maxdepth: 1

guide/guarantees
guide/references
guide/switch
guide/mesh
guide/generics
guide/variadic
guide/shared-inputs
guide/global-state
guide/services
```

```{toctree}
:caption: Task guides
:maxdepth: 1

how-to/choose-operation
how-to/testing
how-to/conversion
how-to/errors
how-to/profiling
how-to/cpp-extension
```

```{toctree}
:caption: API reference
:maxdepth: 1

reference/index
reference/graph-code
reference/nodes
reference/input-views
reference/outputs
reference/types
reference/scalars
reference/literals
reference/testing
reference/execution
reference/state
reference/sources
reference/higher-order
reference/services
reference/operators
```

```{toctree}
:caption: Library reference
:maxdepth: 1

reference/library-index
reference/library-numeric
reference/library-selection
reference/library-stateful
reference/library-time
reference/library-collections
```
