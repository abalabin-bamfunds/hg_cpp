# Global state

## Purpose

`GlobalState` stores scalar values shared by the outer graph and its inner
graphs. Nodes use it for data and side effects that must outlive one node or
graph invocation.

One outer graph evaluation uses one `GlobalState`. Every evaluation context in
that evaluation provides the same object. `run_graph(function, state)` accepts
an object that can outlive the run.

## Values

Each entry maps a symbol to an owning `ScalarValue<>`. Setting an entry copies
the value into the global state's memory resource. Replacing or removing the
entry ends the lifetime of views into its value.

`read()` returns a scoped read access object. Its `at()` function returns a
borrowed `Scalar<>` that remains valid for the scope. `write()` returns a scoped
write access object and activates the global state's allocation context.
`mutable_at<T>()` returns a mutable collection view from a write scope after
checking its type.

Use `scalar_cast<T>()` to check and narrow a view. Use
`unchecked_scalar_cast<T>()` only when the caller has already proved
compatibility.

Global state mutations are not transactional. A mutation remains visible if a
later node fails.

## Concurrency

Read scopes can run concurrently. A write scope excludes other read and write
scopes.
