# All-valid input checks

Status: accepted

## Context

Collection root validity does not imply immediate item validity. The node
runtime must enforce an `all_valid` input constraint even when the input view
uses compile-time layout information and stores no runtime type pointer.

## Decision

Store each `all_valid` input's declared semantic type and runtime type in a
separate compiled-node list. This is required to check the declared fields of a
narrowed bundle using its actual storage layout. Preserve the runtime type list
used to construct input views. Use spare bits in parameter bindings and
subscriber records to identify the constraint and refresh its cached result
when immediate items can change validity.

## Consequences

Checks add compiled metadata only to nodes that need them. Runtime parameter
bindings and subscriber records retain their sizes, and collection items gain
no validity-check storage. Fully known input views keep their direct access.

See [validity](../time-series-validity.md#nodes) and
[node layout](../node-layout.md#compiled-node).
