# Scalar and time series literals share a carrier

Status: accepted

## Decision

`ScalarLiteral` and `TimeSeriesLiteral` name the same borrowed recursive carrier.
Its leaves carry scalar sources. Scalar construction and time series application
retain separate operation records and interpretation rules. See the
[literal API](../scalar-api.md#literals).

One registered scalar type, `Sentinel`, carries a qualified symbol identity and
an optional borrowed literal argument. BG exports its built-in instances from
the engine library. Extensions define additional instances and interpret them
in their literal consumers. Consumers interpret instruction placement and
arguments. See the [sentinel contract](../scalar-api.md#sentinels).

## Reason

Structured scalar replacements inside time series must reach scalar extension
handlers directly. One recursive carrier preserves borrowed storage throughout
that call without converting or copying the literal tree.

Symbol identities allow extensions to add instructions without changing BG's
literal representation or registering another scalar type. Exported constants
and canonical scalar registrations give libraries shared BG definitions.

This replaces the separate-carrier decision in
[0062](0062-scalar-literal-consumption.md).
