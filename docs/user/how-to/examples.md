# Runnable examples

To run an example outside the documentation, download its complete Python file or C++
project. Python files include their imports, definitions, and invocation; C++ archives
include `main.cpp` and a CMake project linked to the required HG targets.

Python files use an [installed HG package](../guide/installation.md). For a C++
archive, extract it and use the **Build and run** instructions in its README.
The [C++ example project](../guide/installation.md#c-example-project)
shows how to locate the SDK installed by pip.

The examples use finite inputs and print their results, while the application recipes
show how several operations work together:

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
