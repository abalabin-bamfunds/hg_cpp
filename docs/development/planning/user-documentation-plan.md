# User documentation plan

Status: approved structure; implemented in the user documentation lane.

Audience: readers who know basic Python or C++ and are new to computation graphs.

## Structure

The quick starts provide compact syntax examples and links to detailed explanations.
The graph guide describes patterns for connecting calculations. The node guide
explains how to implement calculations. Advanced guides introduce time series
contracts when a requirement needs more than ordinary values.

| Section | Purpose |
| --- | --- |
| Getting started | Recognise the syntax and run a small graph. |
| Writing graphs | Express calculations using graph patterns and existing operations. |
| Writing nodes | Add calculations written in ordinary Python or C++. |
| Advanced syntax at a glance | Find compact examples of explicit types and runtime controls. |
| Advanced graphs | Express independent updates, changing dependencies, and stronger type contracts. |
| Advanced nodes | Inspect and control validity, activity, publication, state, and scheduling. |
| Execution and diagnostics | Connect applications, test behavior, and investigate failures. |
| Reference | Find complete API contracts and library operations. |

Each graph pattern has one primary chapter. That chapter covers the relevant
approaches together and explains when each applies. Advanced chapters explain
additional contracts and link back to the pattern.

## Table of contents

### 1. Getting started

#### 1.1 BG overview

- What BG computes and where a computation graph is useful.
- Retained intermediate results and dependency tracking reduce recalculation.
- Nodes, connections, inputs, and outputs, shown in code and a small diagram.
- Graph construction and graph evaluation, in one short explanation.
- Python and C++ authoring options.

#### 1.2 Installation

- Python installation and import check.
- C++ installation and minimal project.
- Running a supplied example.

#### 1.3 Basic syntax crash course

Purpose: provide a compact, runnable introduction before the detailed guides.

- Fixed values and a first result.
- Arithmetic and named expressions.
- A graph function with parameters and a return value.
- Calling another graph function.
- Changing inputs supplied as sequences to `run_graph`.
- A conditional expression.
- A small node written with plain value types.

Each item has a short explanation, code, a result, and a link to its detailed
chapter. Fixed-value syntax precedes changing values. Plain types remain the
default; annotations required by the C++ graph boundary receive a local explanation.

### 2. Writing graphs

This guide organises existing operations by the calculation they express.
Examples use plain value types wherever the supported API permits them.

#### 2.1 Expressions and composition

Purpose: combine existing calculations into a larger calculation.

- Arithmetic, comparisons, and named intermediate results.
- Graph functions, parameters, fixed arguments, and results.
- Calling graph functions and sharing intermediate calculations.
- Construction-time values and values supplied during evaluation.

Example: a price calculation reused to compute a subtotal and a final total.

#### 2.2 Types and connections

Purpose: connect compatible outputs and inputs and identify incorrect connections.

- Supported scalar types and plain annotations.
- Output-to-input type matching in the simple, concrete case.
- Type inference and explicit declarations where required.
- Operator selection from argument types.
- Explicit value conversion and connection type checks.
- A type mismatch, its diagnostic, and the corrected connection.

Example: connect an integer result to another calculation, then handle a numeric
type mismatch. Generic resolution and guarantees belong to advanced graphs.

#### 2.3 Scalar collections and structured values

Purpose: pass several values as one input or result.

- Supported lists, tuples, dictionaries, and sets.
- Named fields and structured scalar values.
- Item and field selection.
- Whole-value updates and calculations over a scalar collection.
- Fixed collections used to construct several graph connections.

Example: a complete set of prices supplied as one value and reduced to a total.
Per-item time series updates belong to advanced graphs.

#### 2.4 Changing inputs and update patterns

Purpose: define which input changes produce results.

- Test sequences, current values, and positions with no update.
- Multiple inputs, simultaneous updates, and dependency order.
- Repeated values and `dedup`.
- Signal-driven publication with `sample`.
- Input sequences containing scalar collections.

Example: a quote calculation that publishes on price changes or explicit requests.

#### 2.5 Conditionals

Purpose: choose values, forward updates conditionally, or run a selected calculation.

- Native `if` for decisions made during graph construction.
- Comparisons and Boolean combinations of changing values.
- `if_then_else` for value selection.
- `filter_` for conditional forwarding.
- `default_` for a fallback when an input has no current value.
- `switch_` for selecting a graph instance, including cases and a default.
- Branch evaluation, output updates, and the absence of short-circuit behavior
  in value selection.
- Instance replacement and state reset when a switch changes case.
- A comparison table covering purpose, branch execution, retained output, and state.

Examples: a bounded result, enabled publication, a fallback reading, and a selected
operating mode. These examples appear together so the alternatives can be compared.

This chapter owns the complete overview of conditional patterns. Additional library
primitives join the relevant pattern when implemented. Planning records distinguish
supported and pending APIs; published examples use supported APIs.

#### 2.6 Iteration

Purpose: apply a calculation to several items.

- Native loops over fixed values during graph construction.
- Per-key graph application with `map`.
- Fixed arguments and inputs shared across mapped calculations.
- Multiple mapped arguments and key selection.
- Item addition, updates, removal, and independent state.
- Construction-time repetition and runtime graph instances.

Example: apply one calibration graph to several sensors, then add and remove sensors.

The current `map` API operates on keyed time series inputs. Its first example needs
a minimal keyed-input declaration, explained at the point of use. The complete
collection type model, validity rules, references, and subscriptions belong to
advanced graphs. Examples must not imply that `map` accepts scalar containers.

#### 2.7 Aggregation and history

Purpose: combine items or retain information across successive updates.

- Built-in reductions over the current collection.
- Custom aggregation with `reduce`.
- Counts, previous values, running totals, and running means.
- Reset signals.
- Aggregation across items and accumulation across time.

Examples: a current portfolio total and a running traded-volume total.

### 3. Writing nodes

This guide implements value calculations with ordinary language code. Graph patterns
remain in the graph guide.

#### 3.1 Calculation nodes

- Plain input and result types.
- Fixed parameters and changing inputs.
- Ordinary arithmetic, native conditionals, loops, and helper functions.
- Nodes consuming and returning scalar collections.
- Calling a node from graph code.

Example: an application-specific calculation unavailable in the library.

#### 3.2 Results and evaluation

- Reading current input values during evaluation.
- Required inputs and the first evaluation.
- Publishing a result or skipping publication.
- Exceptions and evaluation failure.
- Testing a node through a small graph.

Example: a calculation that publishes only accepted readings. Explicit optional
inputs, invalidation, and update inspection belong to advanced nodes.

### 4. Advanced syntax at a glance

Purpose: provide a compact syntax reference for readers entering either advanced guide.

- Plain types and explicit `TS` declarations.
- Scalar collections and independently updating time series items.
- Optional inputs and input views.
- Validity, modification, and input activity.
- References and time series guarantees.
- State and scheduled evaluation.

Each entry contains the smallest useful example, its effect, and a link to the
full explanation. This page is an optional entry point after the basic guides.

### 5. Advanced graphs

#### 5.1 Time series types

Purpose: describe update behavior that a scalar type alone cannot express.

- Current value and update time; history requires a calculation.
- Plain annotations and explicit time series declarations.
- No update, no current value, and invalidation.
- Time series output-to-input compatibility.

Example: an input loses its current value and later becomes valid again.

#### 5.2 Time series collections

Purpose: update and subscribe to individual items independently.

- A scalar collection held in one time series and separately changing items.
- Lists: fixed positions and sparse indices.
- Bundles: named items with different types.
- Dictionaries: changing keys and per-key values.
- Time series sets: membership changes.
- Item selection, root validity, item validity, and change information.
- Effects on the iteration and aggregation patterns.

Example: one sensor changes without republishing every sensor value.

#### 5.3 References and dynamic binding

Purpose: follow a selected source as the source identity changes.

- Value identity and source identity.
- References, dereferencing, and retargeting.
- Dynamic item selection and dependency subscriptions.
- Invalid targets, removed items, and recovery.
- Reference behavior behind conditionals, `map`, and `switch_`.

Example: a calculation follows whichever sensor is currently selected.

#### 5.4 Activity and dependency patterns

Purpose: control which changes trigger a calculation.

- Active and passive connections from the graph author's perspective.
- Sampling and explicit trigger dependencies.
- Dynamic subscriptions and selected collection items.
- Per-key dependencies with `mesh`.

Examples: a requested snapshot and calculations whose keys depend on other keys.
Advanced nodes owns the input-view APIs and evaluation controls.

#### 5.5 Guarantees and advanced type matching

Purpose: declare publication constraints and reuse graph code across compatible types.

- Ordinary, deduplicated, final, and const time series.
- Guarantee compatibility and propagation through a graph.
- Collection and reference guarantees.
- Generic types, constraints, and variadic parameters.
- Reusable operators and overloads.
- Runtime type dispatch and its relation to value-based conditionals.

Examples: a calculation accepting several numeric types and a single-publication input.

#### 5.6 Shared dependencies and services

- Explicit arguments and context time series.
- Shared scalar state with `GlobalState` where supported.
- Services and implementation selection.
- Ownership and lifetime across inner graphs.

Example: several calculations use a shared lookup service.

### 6. Advanced nodes

#### 6.1 Input views, validity, and activity

Purpose: inspect input state when current values alone are insufficient.

- Current value, validity, modification, and last update time.
- Optional inputs and fallback logic.
- Required, optional, and `all_valid` constraints.
- Active and passive inputs; activity and validity are independent controls.
- Input-view lifetime.

Examples: identify which input changed and calculate when only some inputs are valid.

#### 6.2 Collection inputs and output publication

Purpose: process changes without reconstructing complete collection values.

- Updated items, added and removed keys, and set membership changes.
- Item validity and `valid_items` where supported.
- Complete values and change information.
- Publishing, skipping, and invalidating output.
- Collection output updates and guarantee enforcement.
- Python and C++ API differences.

Example: update an aggregate from changed items and remove contributions for deleted keys.

#### 6.3 State and lifecycle

- Retained node state and initialization.
- Separate state for independent instances.
- Start and stop callbacks.
- Resource ownership and cleanup.
- State under mapped and switched graph lifetimes.

Example: an accumulator with an explicit reset and independently owned instances.

#### 6.4 Time, scheduling, and custom sources

- Evaluation time and scheduled evaluation.
- Timed output without a new input update.
- Custom finite sources.
- External producers and queues.
- Scheduling and cleanup when an inner graph stops.

Example: a source publishes timed values and releases an external subscription on stop.

### 7. Execution and diagnostics

- Sources, sinks, and complete applications with `run_graph`.
- Simulation and real-time execution.
- Library time patterns: periodic signals, delay, and throttle.
- Tests for updates, invalidation, time, state, and dynamic graph instances.
- Type errors, graph-construction errors, and evaluation failures.
- Profiling and interpretation of reported measurements.
- Python graphs with C++ nodes.
- Complete application examples and downloadable projects.

Split these topics into a few task-focused pages. Crash-course tests remain
sufficient for running the earlier graph and node examples.

### 8. Reference

#### 8.1 API reference

- Graph declarations, signatures, connections, and type matching.
- Node declarations, input views, outputs, state, and lifecycle.
- Scalar types, time series types, references, and guarantees.
- Iteration, conditional graphs, dependent graphs, context, and services.
- Execution, scheduling, sources, queues, testing, and literals.
- Language-specific APIs and interoperability.

#### 8.2 Library reference

- Arithmetic, comparison, Boolean, string, and conversion operations.
- Selection, conditional forwarding, and fallback operations.
- Sampling, deduplication, history, and running calculations.
- Time operations.
- Scalar collections, time series collections, and reductions.
- Alphabetical index with links to the relevant graph patterns.

## Page and example format

- Use topic titles and direct descriptions of purpose, behavior, and constraints.
- Give every concept a Python example and a C++ example with observable results.
- Keep quick-start explanations short and link to detailed explanations.
- Begin pattern chapters with the problem and a complete example. Add variants
  and a comparison table where the reader must choose between approaches.
- Use direct Python imports and C++ namespace imports. Keep literal test sequences
  readable. Explain required type declarations at the point of use.
- Synchronize Python and C++ tabs. Place C++ setup in a nested **Build and run** tab.
- Use diagrams for topology, selected dependencies, or instance lifetime when code
  alone leaves the behavior unclear.
- Generate downloads from checked examples. Keep Sphinx and Read the Docs support.
- Separate supported API coverage from pending library work during planning.

## Review checklist before page writing

- Confirm chapter boundaries and quick-start scope.
- Check that each graph pattern covers the relevant library and structural APIs.
- Verify supported syntax in both languages, especially scalar collection operations
  and the smallest `map` example.
- Inventory pending conditional primitives without inventing their public APIs.
- Assign each detailed concept one primary location and link from other uses.
- Review the sequence from plain types to explicit time series contracts.

The implementation review is recorded in [documentation review](documentation-review.md).
