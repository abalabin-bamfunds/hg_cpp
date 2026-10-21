# Selection and validity

These operators decide which values or updates reach downstream calculations. Python
imports them from `bg.operators`; C++ uses `bg::operators` from `<bg/library.hpp>`.
Inputs can include compatible reference connections.

## valid

`valid(value)` reports root validity. It evaluates in the first cycle, accepts initially
invalid inputs, and produces a deduplicated Boolean output. Reference arguments observe
their targets through normal input bindings. Collection root validity is separate from
[item validity](input-views.md#collection-views).

## sample

`sample(signal, value)` copies the complete current value when a valid signal ticks and
`value` is valid. Changes to `value` alone do not schedule evaluation. Invalidating
either input preserves the last sampled output. The output has ordinary guarantees,
including its contained time series, so a later signal can sample an unchanged value
again. Copying uses the normal collection mutation rules: unchanged set membership and
already-valid empty dictionaries do not publish another tick. Contained references copy
reference values, not their target values. See [output copying](outputs.md).

## dedup

`dedup(value)` forwards changes through an output with at least the deduplicated
guarantee. Equality is exact and uses the scalar type's equality operation;
floating-point values use no tolerance. Inputs already observed as deduplicated, final,
or const are returned without adding a node. Invalidation and revalidation follow the
normal output rules. Collection outputs apply the guarantee recursively and retain
stronger contained guarantees.

## filter_

`filter_(condition, value)` forwards changes while the condition is valid and true. It
preserves its output while closed. Opening publishes the current value if it has updated
since the last forwarding; repeated true condition ticks do not republish an unchanged
value. Reopening reconciles the complete collection, including removals while closed.
Input invalidation propagates while open; TSS and TSD outputs become valid empty
collections rather than root-invalid. The output retains the input's root guarantee up
to `final`. Contained time series are limited to that same guarantee because reopening
can copy unchanged items. A const input becomes final because its first forwarding can
be delayed.

## if_then_else

`if_then_else(condition, true_value, false_value)` selects an input by reference.
Changing the condition exposes the selected input's current value. Updates to that input
remain observable without copying. An invalid condition or selected input makes the
observed result invalid. Updates to the other input and repeated identical condition
values do not publish additional ticks. Selection applies to the whole collection. Both
inputs must have a [common time series type](types.md#guarantees).

## all_valid

`all_valid(value)` tests root validity and validity of every immediate child. Its
startup and deduplication rules are the same as `valid`. It does not recursively require
all descendants of a nested collection.

## default_

`default_(value, fallback)` follows `value` while it is valid and supplies the fallback
while it is invalid. It does not make `value` valid. The fallback is a scalar of the
input scalar type; a time series fallback is not accepted. Use an optional node input
for an application-specific fallback rule.
