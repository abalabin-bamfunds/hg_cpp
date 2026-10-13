# Callable output guarantee inference

Status: accepted

## Context

Plain callable parameters already capture each supplied time series guarantee.
Small stateless library calculations should retain those guarantees without
requiring explicit input and output declarations.

## Decision

Infer the weakest observed input guarantee for eligible C++ callables returning
a required scalar. Resolve the guarantee through the existing resolver API.
Use the declared output rules when optionality, dynamic activity, or independent
scheduling prevents that inference.

## Consequences

The same callable handles scalar and time series operands. Output publication
enforces the resolved guarantee without additional runtime metadata. Eligibility
includes the scheduling needed to satisfy a const output's first update.

See [callable output inference](../type-system.md#callable-output-inference).
