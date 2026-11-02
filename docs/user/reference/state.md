# State and lifecycle

State belongs to one runtime node. Use it for data that must survive callbacks without
becoming a time series output.

## State

Python declares `state: State[Factory] = None`. The factory is called without arguments
on the first invocation of that runtime node; the callback receives the object directly.
A node can request at most one state parameter. C++ aggregate fields `State<T>` access
the node's retained value with `*state` or `state->member`. Scalar collections expose
their supported mutable state interfaces.

Each runtime node owns its state. Calls that reuse one wired node share that state.
Python disables wiring reuse for nodes requesting `State` and for generators. C++
disables it with `deduplicate_wiring = false` on a callable node or `static constexpr
bool deduplicate_wiring = false` on an aggregate definition. Separate graph runs and
separate inner graph instances have independent state. Required-input invalidation does
not automatically reset arbitrary state. Define that transition explicitly. `sums`,
`count`, and other library operators specify their own reset rules.

## Lifecycle

Start prepares a node before evaluation. Stop releases resources during cleanup. A
selected inner graph starts on creation and stops on replacement or removal. Stopping a
graph cancels its pending work. C++ stop cleanup must not throw. Python reports stop
exceptions through the unraisable-exception hook after cleanup.

Python registers hooks with `@definition.start`, `.stop`, and `.on_invalidation`. Each
hook selects declared node parameters by name. Injected parameters must be present on
the base declaration. A start hook's return is ignored. C++ aggregate definitions use
`const` lifecycle methods; callable declarations can supply lifecycle options.
Invalidation hooks address required-input invalidation that prevents normal evaluation.

## Shared application state

C++ `GlobalState` stores scalar values shared by the outer graph and its inner graphs.
Supply an object through `GraphRunOptions.global_state` to retain it beyond one run;
otherwise the runner creates one. The caller must keep a supplied object alive until
graph destruction. Python does not expose this API.

`read(callback)` supplies read access; `write(callback)` supplies exclusive write
access. `at()` returns a borrowed scalar valid within the callback. Store owning
`ScalarValue` objects when a value must leave it. Replacing or removing an entry
invalidates views of that entry. Mutations are not transactional and remain visible if a
later node fails.

Use node state for local history, time series connections for changing graph
dependencies, and [services](services.md) for shared graph implementations.
