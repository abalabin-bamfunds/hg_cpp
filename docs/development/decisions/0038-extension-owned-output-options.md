# Extension-owned output options

Status: accepted

## Context

A time series extension can provide several output interfaces and storage
representations for one semantic type. Built-in time series do not need the
extension's configuration vocabulary.

## Decision

Providers define configured `Out` specializations and their option semantics.
The extension-facing `output_declaration_traits` identifies the semantic type
and selects the output runtime. Node invocation binds the complete output
declaration. Selection receives the resolved semantic type independently of
input representations.

## Consequences

BG core supports configured outputs without defining frame-specific options.
Node code uses the provider's typed mutation operations. Provider declarations
control both output interfaces and storage.

This retains provider ownership while replacing the core-defined output
specifications in [provider-owned time-series runtimes](0027-provider-owned-time-series-runtimes.md).
See [output declarations](../design/time-series-providers.md#output-declarations).
