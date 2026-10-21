# User documentation plan

Audience: readers who know basic Python or C++ and are new to computation graphs.
Chapters answer concrete graph-authoring needs. Each feature explains its purpose,
shows supported syntax, and links to its API contract.

## Documentation structure

| Area | Purpose |
| --- | --- |
| Getting started | Explain BG, install it, and run a fixed-value program. |
| Guide | Introduce concepts with progressively more syntax and behavior. |
| Task guides | Answer specific testing, conversion, diagnostic, profiling, and integration questions. |
| API reference | Specify graph-author declarations, runtime access, types, execution, and services. |
| Library reference | Group supplied operations by behavior and provide an alphabetical index. |

Engine design documents remain outside the published site. Each concept has one
primary explanation. Guide pages explain the behavior needed for their examples;
reference pages own the complete contracts.

## Presentation rules

- Use documentation language: purpose, syntax, behavior, constraints, and results.
  Keep commands for installation and build/run instructions.
- Begin with plain values. Introduce time series annotations when views,
  collections, guarantees, or generic signatures require them.
- Provide Python and C++ examples with equivalent observable behavior. State
  language differences where the APIs differ.
- Use direct Python imports and C++ namespace imports. Use literal test sequences
  and sentinel values, with no unnecessary optional arrays.
- Keep language tabs synchronized across pages. Put C++ setup in a nested
  **Build and run** tab; select **Code** by default.
- Use temperature readings in the core sequence, then multiple sensors for
  collections. Keep example names and diagrams consistent.
- Label fragments with their dependencies. Supply complete examples for new tasks.

## Concept sequence

### Getting started

Introduction to BG → installation → first program.

The first program introduces imports, declarations, `const_`, a return value,
and `test_graph` without input sequences. It assumes no graph knowledge.

### 1. Graph basics

Arithmetic → graph functions → changing values → conditionals.

Expressions and named intermediate results establish dependencies. Functions add
parameters and composition with fixed arguments. Changing values introduces test
sequences, retained values, skipped/repeated updates, input availability, and
simultaneous updates. C++ first uses `wp<TS<i64>>` here.

Conditionals receives a full chapter because construction-time control flow and
evaluation-time value selection have different behavior. It covers native truth
conversion, fixed settings, Boolean combinations, multiple outcomes, selected
input updates, and the absence of short-circuit evaluation.

### 2. Working with updates

Deduplication → conditional forwarding → sampling → count/lag → running totals/means.

Each operation changes which updates reach later calculations or how updates
contribute to history. Library calls precede custom implementations. This section
requires neither input views nor custom node state.

### 3. Writing nodes

Plain calculation nodes → skipped output → optional inputs → invalidation and
recovery → input views → input availability.

Ordinary node control flow works on values. No publication, absent input,
invalidation, and a skipped test position have separate explanations. Zero-valued
inputs demonstrate why absence checks cannot use truthiness.

Input views follow a task requiring update information: identify which of two
inputs changed. Explain `.value`, `.modified`, validity, and callback lifetime.
Python uses plain optional inputs; C++ also provides optional input views.

### 4. Running graphs

Sources/sinks and `run_graph` → evaluation time → periodic signals → delay →
throttle → reset history → custom state → lifecycle → timed sources → queues.

Finite simulation examples establish termination before external producers or
realtime execution. Evaluation time precedes timers. Library history precedes
custom state. Node state and lifecycle precede resource-owning sources.

C++ aggregate declarations expose state and evaluation context; Python injects
state, clocks, and schedulers. Wiring reuse is distinct from value deduplication.

### 5. Collections

Fixed lists → bundles → dictionaries → sets → current-item reductions → map →
custom aggregation.

Each shape introduces its own declaration, delta, item selection, and availability
rules. Current-item aggregation is distinct from history over successive updates.
Boolean reductions follow numeric reductions. Map introduces independent graph
instances per key; custom reduce follows the built-in operations.

### 6. Advanced graphs

Guarantees → references → switch → mesh → generics → variadics → shared inputs →
application state → services.

Single-publication sources and deduplication motivate explicit guarantees.
Dictionary selection motivates reference identity and retargeting. Stateful nodes
make switch lifecycle observable. Map and references precede mesh dependencies.
Generic types precede variadic capture. Explicit dependencies, context inputs,
shared scalar storage, and service implementation selection address distinct needs.

## API compatibility

Examples use the lane's current implementation and tests. Design documents are
supporting sources; examples must not invent syntax from a proposed interface.

| Facility | Python | C++ |
| --- | --- | --- |
| Plain optional node inputs | `int \| None` | `std::optional<i64>` |
| Skip publication | Return `None` with a plain output annotation | Return a disengaged optional or leave `Out` unchanged |
| Optional input views | Not exposed | `In<TS<i64>, optional>` |
| Retained node state | `State[Factory]` injectable | Aggregate `State<T>` field |
| Evaluation clock | `EvaluationClock` injectable | Aggregate callback context |
| Custom active/passive inputs | Use library sampling | Input declaration properties |
| Explicit output mutation | Return a delta; `Output` is read-only | `Out<>` operations |
| Custom collection aggregate | Collection node | Reducer graph or collection node |
| Context inputs | Explicit arguments | `with_context` and context parameters |
| Application scalar storage | Application-owned sink storage | `GlobalState` |
| Operator declarations | Export a native operator | `operator_`, `overload`, registration groups |

These differences do not authorize engine changes. Python alternatives show the
same application result where a direct API is unavailable.

## Review and improvement sequence

At least four distinct cycles review the complete body and apply corrections:

1. Structure and prerequisites: concept order, chapter purpose, overlap, and reference links.
2. API and behavior: executable examples, declarations, defaults, validity, state, and lifetime.
3. Readability and consistency: syntax load, terminology, purpose, repetition, and documentation tone.
4. Publication and retrieval: navigation, fragments, language tabs, hidden setup, search, and layout.

[Documentation review](documentation-review.md) records corrections and evidence.
`docs/check_examples.py` runs the complete examples in both languages. The native
extension task is built separately. Sphinx treats warnings as errors.

## Draft status

The guide, task guides, API reference, and library reference are drafted on
`agent/user-documentation`. The opening chapters retain their established URLs.
The lane owns its build artifacts. Integration into the root worktree requires
owner approval under the local agent workflow.

## Documentation site and publication

Sphinx, MyST Markdown, and Furo provide navigation, search, and language tabs.
[Read the Docs configuration](../.readthedocs.yaml) uses the same source and
dependencies. [Authoring instructions](README.md) describe local builds, preview,
and example verification. Hosting requires importing the repository into a Read
the Docs project and selecting the branch and versions to publish.
