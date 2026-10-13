# Scalar presence in overload selection

Status: accepted

## Context

Plain node parameters can resolve as scalars or time series. Required and
optional implementations must have consistent selection when a value is
supplied as a scalar.

## Decision

Reject absent scalar arguments for parameters that require presence before
ranking overloads. For equivalent parameter types and a present scalar, prefer
the parameter that requires presence. Combine this preference with time series
validity preferences using the existing partial order.

## Consequences

One plain required implementation handles both scalar and time series calls.
Conflicting presence and validity preferences remain ambiguous.

See [optional arguments](../operators.md#optional-arguments).
