# Shared bundle schema construction

## Decision

One C++ schema can describe both `Bundle<Schema>` and `TSB<Schema>`. The scalar
form requires supported scalar fields. The time series form preserves declared
time series fields and projects each supported scalar field `T` to `TS<T>`.

`bundle<Schema>(arguments...)` selects scalar construction when all arguments
are scalars and both forms are valid. A time series argument selects TSB
construction. `bundle<TSB<Schema>>(arguments...)` explicitly selects TSB
construction, including an all-constant TSB.

## Context

Using one schema keeps field order, names, inheritance, and narrowing identical
between a hierarchical scalar and its field-wise time series form. An
`optional` field may be absent from the hierarchical scalar; in a TSB it remains
a structural field and configures the typed input view. Explicit TSB selection
avoids return-type-dependent overload behavior when scalar arguments make both
interpretations valid.
