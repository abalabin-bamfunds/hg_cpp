# Global state

## Purpose

`GlobalState` stores scalar values shared by the outer graph and its inner
graphs. Nodes use it for data and side effects that must outlive one node or
graph invocation.

One outer graph evaluation uses one `GlobalState`. Every evaluation context in
that evaluation provides the same object. The outer graph's caller owns it and
keeps it alive until the runtime graph is destroyed. `run_graph` and
`testing::test_graph` borrow the object selected by `GraphRunOptions::global_state`.
When that field is null or options are omitted, the runner creates one for the run.

## Values

Each entry maps a symbol to an owning `ScalarValue<>`. Setting an entry copies
the value into the global state's allocator. Replacing or removing the
entry ends the lifetime of views into its value.

`read(callback)` invokes the callback with read access. Its `at()` function
returns a borrowed `Scalar<>` for use within the callback. `write(callback)`
invokes the callback with write access and activates the global state's
allocation context. `mutable_at<T>()` returns a mutable collection view after
checking its type.

Values that leave a callback require an owning `ScalarValue`. A callback must
not retain borrowed scalar or collection views.

```cpp
state.read([](const GlobalState::ReadAccess &values) { /* read values */ });
state.write([](GlobalState::WriteAccess &values) { /* modify values */ });
```

Use `scalar_cast<T>()` to check and narrow a view. Use
`unchecked_scalar_cast<T>()` only when the caller has already proved
compatibility.

Global state mutations are not transactional. A mutation remains visible if a
later node fails.

## Concurrency

Read callbacks can run concurrently. A write callback excludes other read and
write callbacks.
