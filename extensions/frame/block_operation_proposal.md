# Shared column block operations

Status: implemented and reviewed, including the
[node and input-binding design](expression_node_proposal.md).

## Purpose

Give column operators one expression contract and one materialisation path.
BG selects overloads and resolves operand and result types. The frame extension
compiles the selected operations into calls over aligned column batches.

`Expression` describes scalar operations, named columns and parameters, and
owned scalar values. It can be constructed without a wiring context. BG
operator overloads combine it with time series in ordinary bundles. Binding
resolves the scalar operations to the column expression contract below.
These declarations are extension-facing APIs for implementing operators and
expression evaluation.

## Typed callback

Extend `ColumnBlockFunction` to declare its result and each operand independently:

```cpp
template <cell_type RESULT, cell_type... ARGS>
struct ColumnBlockFunction {
    using function_type = void (*)(
        std::span<const row_id_t> rows,
        const ColumnOperandBatch<ARGS> &... operands,
        MutableColumnBatch<RESULT> output);

    function_type function;
};

using Add = ColumnBlockFunction<double, double, double>;
using Less = ColumnBlockFunction<bool, double, double>;
using Negate = ColumnBlockFunction<double, double>;
```

The implementation supports unary and binary callbacks. Add, subtract,
multiply, and divide use matching `i64` or `double` operands. Division returns
`double`. All six comparisons use matching `i64` or `double` operands and
produce nullable Boolean columns. Explicit conversion supports `i64` to
`double` and `double` to `i64`. Operand types must match the selected BG
overload; arithmetic does not implicitly promote mixed types.

`ColumnOperandBatch<T>` remains a typed column batch or scalar broadcast.
`MutableColumnBatch<T>` remains caller-owned output storage. Each column batch
has one cell per supplied row ID. A broadcast supplies one value to every row
without allocating a column. The callback receives borrowed views valid only
for that call and must not retain them.

Callbacks write the validity bit for every output row and the value for every
valid output row. Arithmetic and comparisons propagate null operands. Integer
addition, subtraction, and multiplication wrap modulo 2^64. Division converts
both operands to `double`; a valid zero divisor throws `std::domain_error`.
Conversion to `i64` truncates finite values toward zero, throws
`std::domain_error` for NaN, and throws `std::overflow_error` outside the `i64`
range. The callback owns null handling so operations such as null tests can
define different behaviour. No universal input-validity mask is imposed by the
evaluator. Invalid BG time series inputs follow BG's normal node validity rules.
Callbacks must check `is_valid(position)` before reading a column operand with
`value(position)`. Scalar broadcasts are always valid.

## Semantic and runtime types

`ColumnBlockFunctionType` remains a registered BG `ScalarType`. Extend its
identity from the result type to the ordered operand cell types and result
cell type. These are references to BG's canonical `ScalarType` objects.

The extension's scalar runtime adapter provides the compiler with an erased
block entry point. Each instruction retains the callback in an immutable
`ScalarValue<>` owned by `CompiledExpression` and caches its block adapter.
The signature-specific entry point reads the
retained `ColumnBlockFunction<RESULT, ARGS...>`, constructs typed batch views,
and calls its typed `function` pointer. Function pointers retain their actual
C++ types; they are not cast to object pointers or incompatible function types.

The callback's runtime type owns interpretation of its stored value. The
semantic type describes operand and result types. Wiring checks type
compatibility; evaluation uses compiled bindings without repeating type
matching or resolving overloads. Each instruction calls its erased entry point
once per block.

## Expression bundle contract

Keep immediate operands in an ordinary BG TSB. Unary and binary forms are:

```text
UnaryColumnExpression<ARG, RESULT> {
    arg: ARG
    compute: TS<ColumnBlockFunction<RESULT, cell(ARG)>, const_>
}

BinaryColumnExpression<LHS, RHS, RESULT> {
    lhs: LHS
    rhs: RHS
    compute: TS<ColumnBlockFunction<RESULT, cell(LHS), cell(RHS)>, const_>
}
```

Here `cell(TSC<T>)` and `cell(TS<T>)` both mean `T`. This notation describes
the schema; it does not introduce another declaration type.

The common contract is ordered operand fields followed by a constant `compute`
field. Operand declarations are `TSC<T>` or `TS<T>`. They retain the immediate
operand types and original deferred source identities. They do not recursively
embed upstream expression schemas.

The expression builder establishes these conditions:

- The final field is `compute`, created from an inspectable constant callback.
- The number of operand fields matches the callback's operand count.
- Each operand is `TSC<T>` or `TS<T>` with the callback's corresponding cell type.
- The callback result agrees with the deferred point's `TSC<RESULT>`.

The compiler recognises this contract through the TSB schema and
`ColumnBlockFunctionType`, rather than an enumeration of known operator or
expression types. The shared builder offers one expression form and one
concrete form for each deferred column point.

## Compilation and execution

The shared expression builder attaches the expression form and the concrete
materialisation recipe. It declares its dependencies using BG's existing
materialisation API and creates `compute` with `operators::const_` so the
compiler can read each operation's typed callback during wiring.

Compilation runs inside the selected concrete materialisation recipe after
BG records concrete demands. The compiler processes the root and upstream
operations through the same contract. It stops at concrete materialisation
demands, deduplicates concrete column and parameter inputs, and emits
instructions in dependency order. Shared expression children reuse their
instruction result.

Instructions identify their operands, result buffer, retained callback, and
block entry point. Compilation binds batch adapters for the resolved BG cell
types and uses those adapters' buffer sizes and alignments. `CompiledExpression`
retains no wiring points. Evaluation supplies current column batches and
parameter values.

The expression computation node owns input preparation, changed-row selection,
and output publication. Existing shared-selection and delta-continuity rules
continue to apply. Parameter updates recompute current rows; column updates
recompute affected rows. Each column expression has at least one concrete
column input, which supplies its row domain. BG owns scheduling and input validity.

Heterogeneous columns and captured time series can be declared in ordinary BG
TSBs with wiring-defined schemas. Their fields remain explicit BG dependencies.
Parameter collection uses a separate node and publishes a scalar
`Bundle<SCHEMA>` that preserves each parameter's scalar type.
The collector requires `all_valid` on its immediate captured time series and
publishes one bundle update when any captured time series changes.
The accepted node signature and compiled input bindings are defined in the
[node and input-binding proposal](expression_node_proposal.md).

## Operator registration

Each operator supplies its callback and BG operator registration. The
`ask - bid < tolerance` expression, where `tolerance` is `TS<double>`, uses the
same compiler and computation path as arithmetic.

Boolean storage is implemented by the frame types. Expression-based filtering
uses this block-operation contract; see [filter_proposal.md](filter_proposal.md).
