# Hierarchical scalar semantic types

## Decision

`Bundle<Schema>` declares a hierarchical scalar independently of its future
storage. `HierarchicalScalarType` owns its canonical nominal identity, ordered
field metadata, optional-field metadata, and declared base relationship.

Hierarchical scalars and time series bundles use the shared `schema_reflection`
and `schema_extends` mechanism. `bundle_extends` remains a compatibility alias.

Compatibility is covariant for `List` items and `Dict` values. `Set` items and
`Dict` keys are invariant because their runtime operations require one hash and
equality domain.

## Consequences

The semantic type can be registered and used in type matching before a runtime
type exists. `Bundle` declarations are not scalar values or time series values
until storage and runtime access are implemented.
