# Scalar API

## Purpose

The scalar API separates read-only access from ownership. It also gives generic
node code access to a value without fixing its resolved scalar type.

## Read-only access

`Scalar<T>` is a borrowed, read-only view. Its owner defines its lifetime. A
view supplied to a node is valid for that node invocation and must not be
retained.

A concrete `T` exposes its typed value API. `Scalar<SCALAR>` exposes any
resolved scalar type accepted by that variable without assuming equality or
hashing. `Scalar<Variable>` exposes either operation only when the variable
requires it. `Scalar<>` is a view erased independently of a type variable.

`scalar_cast<T>()` checks the resolved type before narrowing a view.
`unchecked_scalar_cast<T>()` requires the caller to prove compatibility. Both
operations retain the source lifetime.

## Ownership

`ScalarValue<T>` owns its value and is move-only. Construction from `Scalar<T>`
copies the current value. `ScalarValue<>` owns a value of any resolved scalar
type. It can copy or replace its value from any `Scalar` or `ScalarValue`.

Use `ScalarValue` when a value must outlive a borrowed view. A constructor can
accept a memory resource for owned storage.

An explicit memory resource defines the ownership of dynamic storage. Without
one, dynamic storage created during a node invocation belongs to the runtime
graph, and its `ScalarValue` must be destroyed in that invocation. This is
required for graph memory accounting.

## C++ source values

`ScalarValue` converts supported C++ source types to the engine's scalar types.
Signed and unsigned integers that fit convert to `std::int64_t`. Character
strings convert to `bg::string`, whose read API returns `std::string_view`.
`EvaluationTime` retains its type.

`ScalarValue<Opaque<T>>` owns a copy of private C++ data without adding graph
operations. `T` must be copy constructible and destructible. For `Opaque<T*>`,
ownership of the pointed-to object remains with the caller.

## Lists

`List<T>` is a borrowed view of a concrete, homogeneous scalar list.
`ScalarValue<List<T>>` owns the list and its items.

`List<V>` is a type declaration when `V` is a scalar type variable. It has no
runtime value. Node code uses `Scalar<List<V>>` to access the resolved list.
Indexing it returns `Scalar<V>` for the concrete item at that position.
