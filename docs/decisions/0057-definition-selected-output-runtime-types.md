# Definition-selected output runtime types

Status: accepted

## Context

An output's runtime representation can depend on resolved wiring arguments. The
output declaration's runtime trait cannot select that representation.

## Decision

Aggregate, callable, and runtime-signature definitions may select an output
runtime type from `WiringArguments` during wiring. The selected descriptor
replaces trait selection for that node call and is retained through compilation
and evaluation. Definitions without a hook use the declaration's default.

The selected descriptor must have the resolved output's semantic type, support
its declared `Out` interface, and remain alive while the graph uses it.

## Consequences

BG invokes the hook after overload selection and signature resolution. Evaluation
binds outputs to the retained descriptor without calling the hook again. See the
[output API](../node-api.md#output-parameters) for the hook forms and contract.
