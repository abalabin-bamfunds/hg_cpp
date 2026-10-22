# Download runnable examples

Each Python file contains the imports, definitions, and invocation for one
checked example. Each C++ archive contains `main.cpp` and a CMake project linked
to `bg::library`. Fragment examples include the definitions supplied earlier
on their page.

Python files use an [installed BG package](../guide/installation.md). For a C++
archive, extract it and use the **Build and run** instructions in its README.
The [first program](../guide/getting-started.md#write-and-run-the-example)
explains the CMake installation prefix.

These examples use finite inputs and print their result. The application
recipes combine several operations:

- [Temperature alarm changes](temperature-monitor.md): calibration, comparison,
  and publication on transitions.
- [Requested summaries](requested-summary.md): continuous statistics with
  request-driven output.
- [Per-sensor totals](per-sensor-totals.md): independent history and key lifecycle.

The files below are generated from the same sources used by the documentation
example checks.

```{include} ../_generated/examples.md
```
