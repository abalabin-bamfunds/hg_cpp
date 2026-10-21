# Introduction to BG

BG lets you connect calculations whose inputs change over time. You describe
how outputs depend on inputs. BG evaluates the calculations as updates arrive.

For example, a temperature reading can feed a calibration calculation. When a
new reading arrives, BG calculates the calibrated temperature. You can connect
that result to further calculations and reuse it in several places.

```mermaid
flowchart LR
    reading["Temperature reading"] --> calibrate["Add offset"]
    offset["Fixed offset: 2"] --> calibrate
    calibrate --> result["Calibrated temperature"]
```

The connections form a computation graph. Each node performs a calculation;
its inputs connect it to the values it uses. BG uses those connections to
evaluate calculations in dependency order.

Use BG when derived results need to follow changing inputs. Examples include
sensor calculations, changing prices, and results maintained separately for
many items.

## Start here

Every example has Python and C++ versions. The language tabs remember your
choice across pages. The guide assumes basic knowledge of functions and types
in either language and introduces graph concepts from the beginning.

### Getting started

- [Installation](installation.md): set up BG for Python or C++.
- [A first BG program](getting-started.md): imports, declarations, a fixed result,
  and a test run.

### Graph basics

- [Arithmetic](arithmetic.md): combine values with expressions and name results.
- [Graph functions](connect-calculations.md): give a calculation parameters and
  call it from another graph.
- [Values that change](changing-values.md): supply input sequences to `test_graph`,
  combine fixed arguments with changing inputs, and handle skipped or simultaneous updates.
- [Conditionals](conditions.md): choose during construction or evaluation,
  combine conditions, handle several outcomes, and follow updates from the selected input.

### Working with updates

- [Suppress repeated values](dedup.md): publish changes with `dedup` and choose
  where to suppress repeated updates.
- [Forward updates conditionally](filter.md): forward readings while enabled
  and preserve the last output while forwarding is closed.
- [Read a value when signalled](sample.md): publish the latest value on each
  signal update.
- [Count and remember updates](count-and-lag.md): count readings and read the
  preceding calibrated value.
- [Running calculations](running-calculations.md): maintain totals and means
  across successive readings.

### Writing nodes

[Calculation nodes](calculation-nodes.md) add value computations. [Optional outputs](skip-output.md)
and [optional inputs](optional-inputs.md) address missing updates and values.
[Validity](validity.md) distinguishes absence from a retained current value.
[Input views](input-views.md) then expose update information and
[optional view access](view-validity.md).

### Running graphs

[Sources and sinks](run-graphs.md) form complete graphs. [Evaluation time](time.md),
[periodic signals](schedule.md), [delay](delay.md), and [throttle](throttle.md)
control timing. [Resetting history](reset-history.md), [custom state](state.md),
and [lifecycle callbacks](lifecycle.md) define retained information and resources.
[Custom sources](sources.md) and [queues](queues.md) supply timed or external data.

### Collections

Use [fixed lists](fixed-lists.md), [named bundles](bundles.md), [keyed dictionaries](dictionaries.md),
or [sets](sets.md) for the required shape. [Collection reductions](collection-reductions.md)
summarize current items. [Map](map.md) creates a calculation per key;
[custom aggregation](reduce.md) combines items with application-specific rules.

### Advanced graphs

[Guarantees](guarantees.md) constrain publication. [References](references.md)
follow changing sources. [Switch](switch.md) selects graph instances;
[mesh](mesh.md) resolves dependencies between keys. [Generic signatures](generics.md)
and [variadic inputs](variadic.md) reuse declarations across types and argument counts.
[Shared inputs](shared-inputs.md), [application state](global-state.md), and
[services](services.md) address different application-level sharing requirements.

For a specific task, use the [operation chooser](../how-to/choose-operation.md),
[testing guide](../how-to/testing.md), or [error guide](../how-to/errors.md).
The [API and library reference](../reference/index.md) provides complete contracts.

The examples begin with plain types such as `int`. Each chapter introduces
additional syntax when its examples need it.
