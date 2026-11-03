# State and lifecycle

If a calculation needs to retain data between callbacks without publishing it, use
node state. Each runtime node owns its state for its own lifetime.

## State

To retain a running total, cache, or other callback data, request
`state: State[Factory] = None` in Python. The factory is called without arguments on
the first invocation of that runtime node, and the callback receives the object
directly. A node can request at most one state parameter. C++ aggregate fields
`State<T>` access the node's retained value with `*state` or `state->member`.
Scalar collections expose their supported mutable state interfaces.

If equal calls need independent histories, they must create separate runtime nodes;
calls that reuse one wired node share its state. Python disables wiring reuse for
nodes requesting `State` and for generators. C++ disables it with
`deduplicate_wiring = false` on a callable node or
`static constexpr bool deduplicate_wiring = false` on an aggregate definition.
Separate graph runs and separate inner graph instances have independent state.
Required-input invalidation does not automatically reset arbitrary state, so define
that transition explicitly. `sums`, `count`, and other library operators specify their
own reset rules.

## Lifecycle

For resources that must be acquired before evaluation and released when a node
ends, use start and stop callbacks. A
selected inner graph starts on creation and stops on replacement or removal. Stopping a
graph cancels its pending work. C++ stop cleanup must not throw. Python reports stop
exceptions through the unraisable-exception hook after cleanup.

Python registers hooks with `@definition.start`, `.stop`, and `.on_invalidation`. Each
hook selects declared node parameters by name. Injected parameters must be present on
the base declaration. A start hook's return is ignored. C++ aggregate definitions use
`const` lifecycle methods; callable declarations can supply lifecycle options.
Invalidation hooks address required-input invalidation that prevents normal evaluation.

## Shared application state

If outer and inner C++ graphs need the same application data, use `GlobalState`
to share scalar values between them.
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
