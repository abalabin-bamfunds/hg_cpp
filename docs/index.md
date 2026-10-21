# BG documentation

BG connects calculations whose inputs change over time. The connections define
how outputs depend on inputs; BG evaluates the calculations as updates arrive.

[Introduction to BG](guide/index.md) explains the graph concepts.
[Installation](guide/installation.md) covers setup, and
[A first BG program](guide/getting-started.md) provides a complete runnable example.

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
