# Expression computation nodes and input bindings

Status: implemented and reviewed.

## Purpose

Implement the accepted [block-operation contract](block_operation_proposal.md)
with ordinary BG nodes and generic bundles. Wiring fixes the schemas, cell
types, and adapters. Evaluation binds current values and executes block calls.

## Computation node

Use separate BG schema variables named `COLUMNS`, `PARAMETER_INPUTS`, and
`PARAMETERS`. Their C++ declarations use `BundleSchemaVariableDeclaration`.
The computation node has this declaration:

```cpp
template <cell_type RESULT>
struct ColumnExpressionNode {
    Scalar<Opaque<CompiledExpressionOwner>> expression;
    In<TSB<COLUMNS>, all_valid> columns;
    In<TS<Bundle<PARAMETERS>>> parameters;
    Out<TSC<RESULT>, computed<>> output;
    State<BindingCache *> bindings;

    void start(EvaluationContext *) const;
    void evaluate(EvaluationContext *) const;
    void stop(EvaluationContext *) const noexcept;
};
```

`COLUMNS` captures the ordered concrete column inputs. `PARAMETERS` captures
the scalar snapshot schema. These are independent variables: one schema has
`TSC<T>` fields, and the other has scalar fields. Both schemas are fixed during
wiring. Concrete C++ frame schemas and schemas supplied during wiring use the
same node definition.

The selected operator supplies the C++ `RESULT` to the shared materialisation
function. This keeps the result batch and output writer typed. The expression
can contain intermediates and columns with different cell types. The node
implementation is independent of the operation.

BG applies `all_valid` to the immediate column inputs and `required` to the
parameter snapshot. `start()` allocates binding state; `stop()` releases it.
Evaluation does not retain input views between invocations.

## Parameter collector

```cpp
struct ParameterUpdateNode {
    In<TSB<PARAMETER_INPUTS>, all_valid> parameters;
    Out<TS<Bundle<PARAMETERS>>> output;

    static const Schema &resolve(schema_type<PARAMETERS>, const WiringArguments &);
    void evaluate() const;
};
```

The existing aggregate resolver API supports this output schema calculation.
The resolver reads the resolved input schema and maps each field from `TS<T>`
to scalar `T`, preserving field name and order. It returns an anonymous schema
through BG's canonical schema factory.

During evaluation, the collector reads the immediate scalar inputs, constructs
one bundle with BG's scalar bundle factory, and publishes it through
`output.set_value()`. The bundle owns its values. Any parameter update schedules
this collector; BG handles input invalidation and revalidation.

For example:

```text
collector input:   TSB<{ p0: TS<double>, p1: TS<i64> }>
collector output:  TS<Bundle<{ p0: double, p1: i64 }>>
```

An expression with no captured scalar inputs receives a constant empty bundle
and creates no collector node. Constants can follow the existing `const_` path
and participate as ordinary captured time series.

## Wiring and compiled bindings

The shared concrete materialisation function performs these steps:

1. Compile the selected root expression after BG records concrete demands.
2. Deduplicate concrete columns and scalar time series by their BG source
   identities. Assign each input a field index in its corresponding bundle.
3. Build free TSBs with schemas declaring the observed `TSC<T>` column types
   and `TS<T>` parameter types. BG's ordinary input binding dereferences any
   reference sources. Build the parameter collector when parameters exist.
4. Bind typed input adapters, instruction callbacks, intermediate buffer
   layouts, and the typed result publication call.
5. Wire `ColumnExpressionNode<RESULT>` with the immutable compiled expression,
   column bundle, and parameter snapshot.

Internal field names such as `c0` and `p0` identify dependencies; instructions
use field indices. The compiler validates the expression contract before
creating the runtime computation node. BG's ordinary argument matching and
materialisation validate the node's inputs.

Each compiled column binding records its field index and typed adapter. At
invocation, the adapter obtains that bundle field's `TimeSeriesInputData`,
constructs `In<TSC<T>>`, and reads its extension descriptor. The resolved type
selects the adapter during wiring. Evaluation uses the input's actual runtime
type to read its current representation. It does not repeat semantic type
matching or use runtime downcasts.

Each parameter binding records its snapshot field index and typed read adapter.
Evaluation reads the scalar bundle once and prepares each native broadcast
value once in invocation storage. Block callbacks reuse those values.

The compiled expression owns callback values and buffer layout descriptions.
Binding state stores source fingerprints used to detect replacement and lost
delta continuity. Borrowed descriptors, prepared broadcasts, and intermediate
buffers have invocation lifetime. BG nodes retain the current input values.

## Heterogeneous block evaluation

Generalise the existing aligned-batch traversal to use the compiled typed
column adapters. The existing typed entry points use this same implementation.
One traversal supplies the same row-ID span to every column, including when
their cell types differ.

Preserve direct value batches for columns sharing a physical slot map.
Preserve bounded gathering by row ID when physical maps differ. Both paths
retain the existing shared visible-selection requirement. The generalisation
must preserve the current direct-batch path for double-only expressions.

Add erased forms `ColumnBatch<>`, `ColumnOperandBatch<>`, and
`MutableColumnBatch<>` alongside the typed forms. Their headers carry borrowed
value addresses, row counts, and validity information. An erased scalar
broadcast points to the prepared native scalar value. Validity uses one shared
representation across typed and erased batches.

The callback's signature-specific entry point constructs typed
`ColumnOperandBatch<T>` views and a typed `MutableColumnBatch<RESULT>` from
these headers. Input adapters and callbacks use the resolved types recorded
during wiring. Cell values do not acquire individual scalar handles or type tags.

## Callback adapter registration

Use BG's existing `ScalarDispatch<Handler>` optional-operation facility to
attach the erased entry point to each supported callback signature. The frame
library supplies the shared instance anchor for its handler type. Register
handlers before graph wiring, alongside the corresponding operator providers.
Callbacks with the same signature share one typed adapter; addition and
subtraction do not need separate adapter registrations.

The compiler looks up the adapter using the retained callback's actual scalar
runtime type and stores its stable handler pointer in the instruction.
Evaluation calls the cached entry point with the retained `Scalar<>` callback
and current batches. The typed adapter reads
`ColumnBlockFunction<RESULT, ARGS...>` through the normal scalar API.

Callback values use BG's ordinary generated scalar runtime for storage and
copying. The dispatch facility supplies the extension-specific operation;
`ColumnBlockFunctionType` continues to describe the signature through BG's
semantic types. Adapter lookup occurs during wiring.

## Evaluation and publication

`ColumnExpressionNode::evaluate()` keeps the current responsibility split:

1. Prepare current column descriptors and broadcasts.
2. Determine full or incremental recomputation from input replacement, delta
   continuity, parameter updates, and output validity.
3. Bind output membership to the first concrete column.
4. Execute the compiled expression over the required rows and publish typed
   result batches.
5. Record successful input bindings.

Removal changes output membership through the existing output binder. It does
not require evaluating removed cells. Row selection, null behaviour, and
concrete intermediate reuse retain the accepted block-operation contract.

## Code review scope

The first implementation replaces addition-specific compilation and execution
while retaining double addition as the registered operator. Review the typed
callback provider, generic bundles, shared materialisation, heterogeneous
alignment, and resulting addition registration together.

Verification must retain the existing addition regressions and cover unary
compilation, independently typed operands and results using current numeric
cell types, and heterogeneous parameter snapshots. These cases can use test
operator registrations. Comparison, boolean storage, and filtering
remain later implementation steps.

This proposal uses existing BG extension APIs. Any missing capability requires
discussion before implementation.
