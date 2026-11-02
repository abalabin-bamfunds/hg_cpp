# Schema alias identity

Status: accepted

## Context

Scalar bundles and time series bundles share schema declarations. C++ aliases
preserve the concrete schema type but have distinct alias-template identities.

## Decision

Use the concrete schema instantiated with `schema_identity_field` as the
canonical identity for scalar and time series schema descriptors and for scalar
type registration. Keep projections and schema field layouts unchanged.

## Consequences

Aliases of one declaration share canonical scalar types and schema identities
across scalar and time series projections. Separate schema declarations remain
distinct even when their fields match.
