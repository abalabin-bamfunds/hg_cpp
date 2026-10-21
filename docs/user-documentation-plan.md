# User documentation plan

Support readers who know basic Python or C++ and are new to computation graphs.
Organize chapters around useful tasks. Introduce each annotation when a task
requires information that plain values cannot provide.

The presentation conventions below are established. The detailed sequence is a
proposal for the remaining chapters; it does not change the APIs or publish
unfinished pages.

## Documentation structure

| Area | Purpose |
| --- | --- |
| [Getting started](#getting-started) | Explain BG, install it, and run the first program. |
| User guide | Introduce the programming model through the six sections below. |
| Task guides | Solve specific problems, including diagnostics, testing, and profiling. State prerequisites and link to the relevant guide chapters. |
| Reference | Provide the complete API and library contracts, organized independently of the guide sequence. |

The user guide has six sections:

| Section | Reader's goal |
| --- | --- |
| [Graph basics](#1-graph-basics) | Read expressions, call graph functions, and understand changing values and conditional choices. |
| [Working with updates](#2-working-with-updates) | Control which updates produce results and calculate across successive updates. |
| [Writing nodes](#3-writing-nodes) | Add calculations, handle missing values, and inspect input state. |
| [Running graphs](#4-running-graphs) | Connect sources and sinks, work with time, and manage state and lifecycle. |
| [Collections](#5-collections) | Group changing values and calculate for multiple items. |
| [Advanced graphs](#6-advanced-graphs) | Select graph instances, follow references, and use generic or shared inputs. |

The first three guide sections form the core sequence. The remaining sections
state their prerequisites so readers can choose the relevant topic. Keep a
concept in one chapter within its section; later chapters link to that explanation.

Use these section names in the sidebar, the introduction, and this plan. Add a
navigation group when its first complete chapter is available. Keep future
chapters here until they have publishable content. Preserve chapter URLs when
changing navigation groups.

## Established presentation

- Provide matching Python and C++ examples with equivalent inputs and outputs.
- Start with a fixed value and a graph with no input parameters. Cover arithmetic
  and graph function calls before changing inputs and `test_graph` sequences.
- Use plain value types for the fixed-value examples and literal sequences when
  introducing changing inputs.
- Use direct Python imports and C++ namespace imports. Follow the exact
  conventions in [documentation authoring](README.md#add-pages).
- Keep language selection synchronized across pages. Put C++ setup and commands
  in a separate **Build and run** tab, with **Code** selected by default.
- Use temperature readings through the early chapters. Introduce several sensors
  when independent items become the subject.
- Keep engine design documents separate from the published user documentation.

## Rules for organizing chapters

Order by both new syntax and new behavior. A short function call can still
require a separate chapter if it changes when calculations run or how long an
output remains available.

Each chapter should:

1. Start with a concrete need and the API that addresses it.
2. Provide a complete working example, reusing familiar syntax and explaining new syntax.
3. Show the expected output and explain the relevant behavior. Give each concept
   one primary explanation; later chapters use it without introducing it again.
4. Include a variation only when it addresses another useful need.
5. Link to prerequisites, related tasks, and the complete API contract when available.

Use “To do X, use Y” framing. Explain when to use an operation and how it behaves.
Avoid exercises, prediction prompts, and instructions to edit a previous example.
Keep installation, build, and run steps in their operational sections.

Introduce library operations before custom implementations of their behavior.
Use plain custom nodes before input views, returned values before output
modification, and library stateful operations before custom state. Introduce
one collection type at a time.

Keep the main example small enough to read at once. Put complete imports,
includes, and declarations in the runnable example. Label later excerpts with
the definitions and imports they use. Prefer a complete example for a new task
over instructions for assembling one from earlier snippets.
Do not add helper functions or type aliases solely to hide syntax that the
reader needs to understand.

## Chapter sequence

Each row is a chapter within a guide section. A chapter owns a concept; related
operations can share a chapter when they use the same behavior and syntax.

### Getting started

| Chapter | Task and example | New syntax or behavior |
| --- | --- | --- |
| Introduction to BG | Follow a reading through calibration in a small diagram. | Nodes, connections, and evaluation in dependency order. |
| Install BG | Install the selected language and run the first example. | Setup stays in language tabs. |
| A first BG program | Return one fixed integer and run the graph without input arguments. | Imports, Python `@graph`, a C++ graph function, `return`, `const_`, and `test_graph`. Explain the program structure without update sequences. |

### 1. Graph basics

| Chapter | Task and example | New syntax or behavior |
| --- | --- | --- |
| Arithmetic | Add an offset, multiply the result, and name an intermediate result. | Arithmetic expressions, precedence, connections, and dependency order. Inputs remain fixed. |
| Graph functions | Give calibration plain integer parameters and call it from another graph. | Parameters, fixed arguments, return values, and graph construction versus evaluation. |
| Values that change | Use the calibration graph with a changing reading and fixed offset, then make both inputs change. Include skipped, repeated, and simultaneous updates. | `test_graph` input sequences and recorded outputs, scalar arguments versus changing inputs, current values, and initially invalid inputs. C++ first uses `wp<TS<i64>>` here. |
| Conditionals | Cap a temperature, choose a fixed configuration, combine conditions, and express several outcomes. | Explain why ordinary `if` needs a construction-time scalar and why changing conditions use `if_then_else`. Cover Python truth conversion, C++ `wp<>`, Boolean operations, nesting, selected-input updates, and the absence of short-circuit evaluation. |

These pages are [already drafted](#draft-status).

The first program uses `const_` so its result is a graph value. Arithmetic
examples operate on that value rather than calculating only with language
literals during graph construction. Keep `test_graph` calls free of input
arguments until the changing-values chapter. Its returned result can be shown
without explaining update sequences yet.

The graph-functions chapter uses plain integer arguments and creates a fixed
graph value with `const_` inside the reusable function. Show parameters and
calls before introducing time series or update behavior.

Python graph parameters remain plain `int` when inputs start changing. C++ graph
functions use `wp<TS<i64>>` for changing inputs. Explain this in Values that
change: the parameter identifies a connection used to construct the graph, and
the calculation reads its current value during evaluation. Keep C++ graph
return types inferred. Defer `graph(...)` templates, `In<>`, input properties,
and runtime views until a task requires them.

Small diagrams should match the names in the code. Explain dependency order
through arithmetic and graph calls; leave compilation and storage to engine
documentation.

Conditionals is the first departure from ordinary language control flow. Give
it a separate chapter with the reason before the syntax. Show that a connection
cannot supply a native truth value and that fixed scalar configuration can use
ordinary `if`. Introduce Python's named `and_`, `or_`, and `not_` operations while
combining conditions. C++ Boolean graph operations also have no short-circuit
behavior. Explain selected-input updates using the existing temperature example;
keep this behavior in the same chapter. Keep numeric types consistent and cover
conversions in a separate task guide.

Selecting an input does not stop the computations that produce unselected inputs.
Teach `switch_` later, when starting and stopping a selected graph is the task.
Readers can use value selection without first writing an explicit `REF` annotation.

Library examples reuse the library header and link target from the first
program. Keep build instructions in the **Build and run** tab.

### 2. Working with updates

These chapters use the graph declarations and input sequences from Graph basics.
Keep their focus on update behavior; introduce input views and custom state in
Writing nodes and Running graphs.

| Chapter | Task and example | New syntax or behavior |
| --- | --- | --- |
| Suppress repeated values | Feed repeated readings through `dedup`. | An update can carry an unchanged value; deduplication suppresses equal output updates. |
| Forward updates conditionally | Forward readings while an enable input is true. | `filter_`; closing preserves the last output, and reopening can forward the current input. |
| Read a value when signalled | Capture the latest reading when a separate signal updates. | `sample`; updates to the sampled value alone do not produce an output. |
| Count and remember updates | Count readings, then obtain the preceding reading in a separate example. | `count`, then `lag`; skipped steps do not advance either operation. |
| Running calculations | Sum successive readings, then calculate their mean. | `sums`, then `means`; each update contributes, including repeated values. |

### 3. Writing nodes

#### Values and missing inputs

| Chapter | Task and example | New syntax or behavior |
| --- | --- | --- |
| Write a calculation node | Implement a sensor-specific correction using an ordinary value calculation. | Python `@node`; C++ `node` with plain value parameters. Contrast values received by node code with connections received by graph code. Ordinary `if` works on node values. |
| Leave an output unchanged | Publish a corrected reading only when it passes a check. | Python returns `None`; C++ returns `std::optional<int>` and uses `std::nullopt` to skip publication. |
| Accept a missing input | Use an optional offset while a separate reading input updates. | Python `int \| None`; C++ `std::optional<int>` parameters. Optionality permits evaluation with an invalid input; another update still needs to trigger it. |
| Invalidate and recover | Remove a current reading, observe validity, then supply another reading. | `INVALID`, `valid`, and `default_`, introduced in separate examples. Invalidation removes the current value; skipping an update preserves it. |

Use separate input and output examples for optional types. First show what an
optional return means; then show what an optional parameter means. Include a
zero value so fallback code tests absence rather than truthiness.

The invalidation chapter must distinguish these cases in its update table:

- No new input update: the input keeps its current value.
- No output publication: the output keeps its current value.
- Invalid input or output: there is no current value.
- Absent scalar argument: the value was not supplied during graph construction.

Python's recorded root result uses `None` for both skipped and invalid ticks.
Use `valid` to make validity changes observable rather than infer validity from
that result alone. Link to the literal and testing reference for full encoding
rules. Introduce `opt()` only in a later optional-operator chapter, after ordinary
optional parameters and explicit fallback are understood.

#### Input views

Start this section with a task that plain values cannot answer: determine which
of two valid inputs updated in the current evaluation. Reuse their familiar
addition and input sequences.

| Chapter | Task and example | New syntax or behavior |
| --- | --- | --- |
| Values and input views | Read a value and identify which input updated. | Python `@compute_node`, `TS[int]`, `.value`, and `.modified`; C++ `In<TS<i64>>`, value access, and `.modified()`. |
| Validity in node code | Inspect an optional input before reading its value. | Optional input-view declarations and validity queries. Reuse the earlier absent-input example. |

Show the plain node and the input-view version side by side at the transition.
Explain each changed declaration before adding another property. A `modified`
query describes a valid update in the current evaluation; it must not be taught
as a general invalidation detector. Input views are valid only during the node
callback.

Detailed input activity, collection validity constraints, and explicit output
modification belong in task guides after the relevant prerequisites. They do
not all need to be introduced to teach `.modified`.

### 4. Running graphs

These topics use graph composition and ordinary nodes. Each row states any
additional prerequisites; readers do not need to complete every preceding topic.

| Topic | Prerequisites | Chapter order and syntax introduced |
| --- | --- | --- |
| Run complete graphs | Composition and ordinary nodes | Connect a finite library source to an outputless node, then use `run_graph`. Teach sources, sinks, and termination before configuration objects. |
| Work with time | Running graphs | Introduce evaluation time and durations; use bounded `schedule`, then `delay`, then `throttle` in separate chapters. Explain simulation with a finite example. Custom sources, generators, and scheduling follow only when built-ins cannot perform the task. |
| Keep custom state | Ordinary nodes and running calculations | Implement one small accumulator using `State`, then add a reset input in a separate example. Show independent state in two uses. Introduce lifecycle callbacks when acquiring and releasing a resource. |

After evaluation time and input views are introduced, cover update timestamps:
Python `last_modified_time` and C++ `last_updated()`. Keep this explanation here
with the time operations.

### 5. Collections

Introduce one collection type at a time before operations that create a graph
for each item. Keep dependency ordering between items in Advanced graphs, after
references and custom state.

| Topic | Prerequisites | Chapter order and syntax introduced |
| --- | --- | --- |
| Group changing values | Composition and validity | Contrast one scalar dictionary with separately updating items. Teach a fixed `TSL`, then a named `TSB`, then a keyed `TSD`, each on its own page. Put `TSS` in the membership chapter. Introduce literal updates, item validity, and removal with their respective type. |
| Calculate for many items | `TSD` updates, graph functions, and changing inputs | Start with `sum_` over current items, then `map` for per-sensor calibration. Add and remove keys. Introduce custom `reduce` after built-in reductions; explain its associative and commutative requirement. |

### 6. Advanced graphs

These chapters build on the core guide and the prerequisites listed below.

| Topic | Prerequisites | Chapter order and syntax introduced |
| --- | --- | --- |
| Select a computation | Conditionals, graph functions, and custom state | Use `switch_` to select a graph, then change cases. Use state to demonstrate that returning to a case creates a fresh instance. |
| Follow changing sources | Input views and conditionals | Introduce explicit `REF`, retargeting, validity, and source removal. Revisit the references returned by selection and nested graph operations. |
| Resolve dependencies between items | `map`, references, and per-item state | Introduce `mesh` with one dependency between two keys, then changing dependencies and cycle errors. |
| Reuse code across types | Typed boundaries and collections | Scalar type variables, time series type variables, then variadic parameters and generic collections. Keep each introduction separate. |
| Share application inputs | Running graphs and graph functions | Give context time series, global state, and services separate chapters. Explain their contracts through one concrete use each; task guides cover application-specific integration. |

Time series guarantees follow the operations that motivate them: `dedup` before
an explicit `deduplicated` annotation, and sources that update once or supply a fixed constant before `final` and
`const` declarations. Put complete compatibility and propagation rules in
the type reference.

## API checks before drafting

Use current [node](node-api.md), [graph](graph-wiring.md),
[Python](python.md), and [library](library.md) contracts and working tests as the
basis for examples. Do not infer Python syntax from the C++ API.

The current Python node decorators do not expose custom active/passive input
properties. Teach sampling through the library in both languages. Before
planning matching custom evaluation-control examples, resolve the Python API
coverage with the project owner; do not invent an annotation or silently give
the two examples different scheduling behavior.

Python's injected `Output` exposes the previous output for reading. C++
`Out<TS<T>>` also supports mutation. A future output-modification chapter must
use the operations actually available in each language and demonstrate the same
observable result. It must not imply that Python provides C++ output setters.

These are documentation planning constraints, not approval to extend the APIs.

## Reference and task guides

Keep guide chapters, task guides, and reference independently navigable. Reference
material should be available from the first published chapter that uses an API;
its implementation need not wait until all guide chapters are written.

- Task guides answer one question and state their prerequisites. Cover reading
  wiring and evaluation errors, isolating behavior with `test_graph`, and
  profiling a complete graph here. Link troubleshooting from the first relevant
  chapter.
- API reference covers declarations, graph construction, input and output
  access, execution, testing, types, and diagnostics.
- Library reference groups operations by task and provides an alphabetical
  index. Separate operations over successive updates from operations over
  current collection items.
- Engine documentation remains outside the user guide.

Each reference entry owns its complete contract: purpose, language signatures,
accepted types, defaults, triggering updates, invalid-input behavior, retained
output or state, errors, and a minimal example. Guide chapters explain the behavior
needed for the current example and link to the reference.

## Draft status

All guide sections, task guides, API reference, and library reference are drafted
on `agent/user-documentation`. The opening chapters retain their established
URLs. The published navigation includes complete pages rather than placeholders.

The implementation is checked against `docs/python.md`, `docs/node-api.md`,
`docs/graph-wiring.md`, `docs/evaluation.md`, `docs/library.md`, the collection and
higher-order contracts, and their production declarations and tests. The
language support matrix is explicit where C++ facilities lack a Python API.

## Review and improvement sequence

Perform at least four distinct cycles. Each cycle reviews the complete body and
applies corrections before the next cycle.

1. Structure and prerequisites: order concepts, identify each chapter's purpose,
   remove duplication, and link guide tasks to their reference contracts.
2. API and behavior: execute examples in both languages against the lane's code;
   correct signatures, update behavior, defaults, validity, and lifetime claims.
3. Readability and consistency: remove narrative, imperative lesson language,
   unexplained syntax, terminology drift, and unnecessary repetition.
4. Publication and retrieval: check navigation, reference coverage, internal
   fragments, synchronized language tabs, hidden build instructions, search,
   and desktop/mobile presentation; fix discovered defects and rebuild.

Record concrete corrections and validation in [documentation review](documentation-review.md).
`docs/check_examples.py` validates runnable examples. Sphinx builds with
warnings treated as errors. The lane owns its build artifacts and does not
modify engine behavior to make examples pass.

## Documentation site and publication

The site uses Sphinx, MyST Markdown, and Furo. The root
[Read the Docs configuration](../.readthedocs.yaml) uses the same source and
configuration as local builds. See [documentation builds](README.md) for build,
preview, and authoring commands.

Before publication, verify source installation in a fresh environment, automate
example execution, choose how to maintain reference signatures, and document
version availability. Import the repository into Read the Docs when it is ready
to publish.
