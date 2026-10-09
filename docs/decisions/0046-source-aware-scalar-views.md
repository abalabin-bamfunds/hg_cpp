# Source-aware scalar views

Status: accepted

## Decision

Scalar providers may interpret a `ScalarData` source when producing their
declared C++ read value through `read_value(ScalarData)` and may select the
runtime used by a typed owning copy.
This supports compatible runtime representations whose C++ read layouts
differ from the provider's declared view.

If the selected copy runtime is the source runtime, BG uses the existing
actual-runtime copy or shared-retain path. Otherwise, BG constructs the
selected runtime from the source, which must be compatible with it. Providers
without these hooks retain existing scalar copy and read behavior.

## Consequences

The hook contracts and `ScalarData` lifetime rules are defined in the
[scalar API](../scalar-api.md#scalar-providers).
