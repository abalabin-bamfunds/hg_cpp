# Runnable examples

Each Python file contains the imports, definitions, and invocation for one
checked example. Each C++ archive contains `main.cpp` and a CMake project linked
to its required HG targets. Each download contains a complete program.

Python files use an [installed HG package](../guide/installation.md). For a C++
archive, extract it and use the **Build and run** instructions in its README.
The [C++ example project](../guide/installation.md#c-example-project)
shows how to locate the SDK installed by pip.

These examples use finite inputs and print their result. The application
recipes combine several operations:

- [Temperature alarm changes](temperature-monitor.md): calibration, comparison,
  and publication on transitions.
- [Requested summaries](requested-summary.md): continuous statistics with
  request-driven output.
- [Per-sensor totals](per-sensor-totals.md): independent history and key lifecycle.

The files below are generated from the same sources used by the documentation
example checks.

Extension examples include their own dependencies. Frame projects also require
`-DHG_SOURCE_DIR=/absolute/path/to/hg` to build the extension from source. A dash
in the Python column marks an example whose extension currently has only a C++ API.

```{include} ../_generated/examples.md
```
