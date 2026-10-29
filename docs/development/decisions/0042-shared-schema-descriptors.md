# Shared schema descriptors

Status: accepted; registration transactions superseded by
[single-threaded type registration](0094-single-threaded-type-registration.md).

## Decision

Hierarchical scalar types and time series bundle types use one canonical
`Schema` descriptor implementation. Each family validates its field types and
defines its own compatibility and runtime storage.

A descriptor key includes its name, declaration identity, base descriptor, and
ordered direct fields. C++ projections use the identity of the schema-template
wrapper. Runtime declarations use BG-allocated identities. Field types
distinguish the scalar and time series projections of one declaration.

Schema descriptors created during recursive scalar registration join the
existing [registration transaction](0018-atomic-recursive-semantic-registration.md).

## Consequences

Extensions can use schema variables and programmatic descriptors without
implementing separate schema metadata. Python declarations retain nominal
identity without retaining Python objects in schema registration.

The [type system](../design/type-system.md#schemas) defines the descriptor and
projection contracts.
