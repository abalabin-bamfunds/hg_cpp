# Frame filtering uses HG expression materialisation

Status: accepted

## Decision

Frame filtering is a node built through HG's existing expression and
materialisation APIs. It consumes a `TSC<bool>` predicate and emits an
ordinary `TSF<Schema>` view with the source schema. The node evaluates rows
added or changed in predicate columns, then applies membership changes to its
row selection. It forwards changes in other columns for selected rows without
reevaluating the predicate.

See [filtering helpers](../../README.md#filtering-helpers)
for the public API and incremental behavior.

## Reason

HG owns graph scheduling and expression materialisation. The frame extension
uses those mechanisms to evaluate predicates in blocks while retaining source
row IDs and column storage. Predicate parameters remain ordinary HG inputs, so
their changes schedule the node through normal graph propagation.
