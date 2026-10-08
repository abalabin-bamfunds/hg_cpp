# Frame schema foundation

Status: schema-backed semantic descriptors are committed in `e9f04c74` on
`agent/frame-schema`. The common borrowed read descriptor is committed in
`c3f4fafc` and passes all five extension tests, including Arrow. Shared owning
storage, inherited input matching, and generic node access follow.

## Purpose

Use one frame implementation for concrete C++ schemas, schema variables, and
schemas constructed programmatically. Generic nodes must preserve a captured
schema through ordinary BG type resolution. Native and Arrow representations
must expose the same value and change APIs.

The existing source is preserved in commit `12471d94` on `agent/frame-schema`.
The [shared BG schema foundation](../../docs/type-system.md#schemas) is merged
into main. Frames use its descriptors, reflection, and canonical factories.

## Scope

A schema supplied programmatically during wiring is resolved and fixed for
the graph lifetime. Evaluation-time changes to the column set are a separate
contract. The earlier dictionary-of-whole-columns proposal remains the
candidate representation for pivot columns.

The initial cell implementations remain nullable `i64` and `double`.
Unsupported resolved cell types fail during wiring. This work adds schema
generality rather than additional cell layouts or expression operations.

Frame input inheritance is approved. Concrete matching remains unchanged until
the shared storage and scalar access support inherited inputs safely.

## One implementation

The ordered schema descriptor owns names and cell semantic types. It is
canonical and registered with process lifetime. A C++ schema declaration
constructs this descriptor; a programmatic factory constructs one from fields.

```text
bg::Schema {
    ordered fields { name, Type, optional presence }
    optional named identity and base schema
}

FrameType : ScalarType {
    shared schema descriptor or schema variable
}

FrameSeriesType : FixedItemTimeSeriesType {
    shared schema descriptor or schema variable
    guarantee
}

FrameStorage {
    current row view and row count
    ordered column descriptors
    shared membership changes
    per-column modification information
}

NativeFrame {
    NativeRows
    schema-described column storage
    child publication state
    FrameStorage borrowed descriptor
}
```

`FrameStorage`, `NativeFrame`, and their runtime interfaces become independent
of the C++ schema template. A runtime type computes the column layouts and
operation bindings from the resolved schema. Each column continues to use the
existing typed cell storage and batch operations. Frame mutation, rollback,
membership changes, and publication have one implementation.

Frame construction validates that fields describe supported cell scalar types
and present columns. Cell nullability belongs to column values. The extension
does not implement a separate schema descriptor, lookup, or registry.

The column descriptor milestone adds `ColumnType` and `ColumnSeriesType` shared
by concrete declarations. Their extension-facing factories match and resolve
scalar variables through `TypeMap`. Frame and frame-series descriptors follow
with schema variable matching. Generic node binding becomes available with the
shared runtime storage; the descriptor milestone retains the existing concrete
storage and runtime implementations.

Concrete C++ access resolves column ordinals and cell declarations at compile
time. Generic access resolves them from the schema. Both access the same
storage; obtaining a view allocates no column buffers and copies no cells.
Generic access must not require conversion to a separately stored table.

Native owning outputs and borrowed view outputs retain their existing
different ownership contracts. Both support every resolved frame schema.
Projection and filter bookkeeping also become schema-described shared code.

No per-cell scalar handles or timestamp matrix are stored. Runtime dispatch
is bound per column; batch operations retain typed cell spans. Mutation and
traversal costs must be checked against the existing numeric implementation.

## Identity and matching

Preserve C++ schema identity. Schemas constructed from an anonymous ordered
field list are canonicalized by names and cell types. Programmatic code can
also use the descriptor of a declared C++ schema when it requires that named
identity. Equal field layouts alone do not erase named identity.

`Frame<SCHEMA>` and `TSF<SCHEMA>` capture the same frame schema descriptor.
Reusing the variable requires the same schema. Resolution creates the ordinary
registered `FrameType` or `FrameSeriesType`, not a generated C++ specialization.

Schemas with scalar variables in individual fields also resolve through BG's
type map. The runtime is selected only after all schema fields resolve.

### Inherited inputs

`In<TSF<Base>>` accepts `TSF<Derived>` when Derived declares inheritance from
Base. `Frame<Base>` accepts the corresponding derived scalar type. Typed access
uses Base's declared fields; runtime inspection sees the actual Derived schema.
`value_view()` retains that actual scalar runtime and borrows its storage.

Semantic inheritance determines compatibility independently of native or Arrow
storage. Owning copies from a scalar view or a C++ `FrameView<Base>` preserve
the actual schema and all its columns. An explicit projection constructs a
value containing only the selected fields.

Schema-preserving nodes capture `SCHEMA`, including the supplied schema's
additional fields. Each TSF output uses its resolved schema. Input columns do
not extend the output schema.

An anonymous runtime schema matches a named input by its required field names
and compatible cell types. The input retains the actual anonymous schema.

### Scalar extension integration

Status: source-aware scalar reads and copies are committed in `dbbbe24f` on
`agent/frame-bg-foundation`. Typed time-series reads use that path in `0d208d21`
on `agent/scalar-access-paths`. Neither package is merged into main.

BG's current scalar read path asks the source runtime to construct a C++ view
in storage sized for the declared view. `FrameView<Base>` and
`FrameView<Derived>` can have different layouts. Shared frame storage is required
to construct the declared typed view while retaining the actual scalar runtime.

Optional extension-facing scalar storage trait operations:

```cpp
static value_type read_value(const details::ScalarData &data);
static const ScalarRuntimeType &copy_runtime_type(const details::ScalarData &source);
```

The `read_value` operation constructs the declared view from the source representation.
The copy operation selects the runtime used for an owning copy. Existing traits
retain their current behavior. An owning `ScalarValue<Frame<Base>>` copied from
Derived retains Derived's actual schema and columns. Selecting the source
runtime retains shared immutable storage when supported, or clones through
that runtime. Typed copies from a same-schema Arrow representation use canonical
native storage. Node code uses the scalar and time series APIs rather than
these trait operations.

The remaining BG integration needs are canonical scalar runtime customization
and construction from a borrowed C++ view carrying its actual semantic type.
The proposed extension hooks are awaiting approval. Scalar node parameter reads
use their actual runtime in `20cbf814` on `agent/scalar-access-paths`. The full
BG test suite passes: 1464 tests, including Python tests.

## Node and value APIs

Keep the existing concrete-schema surface: typed columns, `Row<Quotes>`,
`RowPatch<Quotes>`, zero-copy `value_view()`, and typed `In`/`Out` operations.

Add the corresponding generic surface:

| Declaration | Access |
| --- | --- |
| `Frame<SCHEMA>` value | Resolved schema, row count, columns by name or ordinal |
| `In<TSF<SCHEMA>>` | Common BG input queries, current scalar view, row IDs, membership changes, column changes |
| Generic column input | Resolved cell type and checked typed column/batch access |
| `Out<TSF<SCHEMA>>` | Complete row insertion, sparse cell patch, erase, clear, invalidate |
| `Out<TSF<SCHEMA>, frame::view>` | Projection/filter bindings over the same source descriptors |

Generic cell input retains the distinction between null and an omitted patch
field. Validate row shape, field identity, and cell types before mutation.
Frame membership changes remain owned by the frame output. Access to one
column does not authorize independent additions or removals from that frame.

`Column<SCALAR>` and `TSC<SCALAR>` need generic declarations and checked access
so generic frames can expose columns without losing their family. They use
the existing concrete column implementations after cell type resolution.

Arrow import gains a schema-inferred form and a form taking an explicit frame
schema descriptor. Both construct the same frame semantic types. Typed imports
validate against their C++ schema descriptor. Borrowed export and owning copy
continue to use the existing lifetime contracts.

## Extension boundary

Frame semantic types, field validation, storage, column operations, Arrow
adapters, and output options belong to the extension. The common BG descriptor
provides schema identity and field metadata. Report any required BG change for
owner review before implementing it.

BG review item: aggregate scalar arguments without a source-aware read hook
retain the supplied runtime's storage but read it as canonical trait storage.
Different layouts require a BG policy decision. Ordinary typed TS outputs
construct their configured canonical representation; manually bound alternative
TS inputs need the same review.

## Build order and acceptance

1. Verify the existing numeric extension baseline against latest BG (complete).
2. Implement the common schema foundation in BG (merged).
3. Add canonical frame/column semantic descriptors and generic descriptor resolution.
4. Convert frame storage and mutation to the shared runtime layout; retain
   concrete typed access and existing behavior tests.
5. Add generic input/output access, programmatic schema sources, views, and
   Arrow import/export through that same implementation.
6. Verify through an installed BG package with Arrow enabled and disabled.

Required examples include a generic schema-preserving node over a concrete
C++ source and over a programmatic schema source, mixed numeric columns,
zero-column frames, patches/nulls/removals, column notifications, and view
rebinding. Test repeated schema variables, schema mismatch diagnostics,
owning lifetimes, allocation failure cleanup, and canonical type identity.
Retain the current five test executables as regression coverage. Measure the
same native numeric workload before and after changing its storage.
