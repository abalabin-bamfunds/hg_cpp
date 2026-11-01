# Column expressions use HG materialisation

Status: accepted

## Decision

Column operators use HG overload resolution, declared dependencies, and
materialisation options. Concrete column demands define where expression
compilation stops. A shared computation node evaluates the selected operations
over aligned blocks. Captured scalar time series use an ordinary HG node to
publish a typed parameter bundle.

See the [block-operation contract](../../block_operation_proposal.md) and
[node bindings](../../expression_node_proposal.md) for the implemented APIs.

## Reason

HG must retain ownership of scheduling, validity, and intermediate reuse.
Block evaluation reduces intermediate column allocation. Requiring a shared
row domain and visible selection lets operands align through their existing
row mappings; filters maintain their selected rows in source order.
