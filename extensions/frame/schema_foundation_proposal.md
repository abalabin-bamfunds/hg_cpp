# Frame schema foundation

Status: implementation and verification are active in `agent/frame-field-access`.
The extension consumes the installed BG package at `cc4a3f29`. Current checkpoints
and remaining work are recorded in [HANDOFF.md](HANDOFF.md).

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

Frame and TSF input matching check named ancestry and required field types.
Declared fields control typed access; the scalar view retains the actual schema.

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

FrameReadStorage {
    current row view and row count
    ordered column descriptors
    shared membership changes
    per-column modification information
}

NativeFrameStorage {
    resolved schema layout
    NativeRows
    schema-described column storage
    child publication state
}
```

`FrameReadStorage`, `NativeFrameStorage`, and their runtime interfaces are
independent of the C++ schema template. Typed adapters hold column read bindings
and translate row arguments. A runtime type computes the column layouts and
operation bindings from the resolved schema. Each column uses the existing typed
cell storage and batch operations. Frame mutation, rollback, membership changes,
and publication have one implementation.

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

The extension declares `supports_alternative_layouts` and implements typed
`read_value(ScalarData)` to construct the declared `FrameView` from the actual
representation. Every frame scalar runtime implements
`FrameScalarRuntimeType::read_storage`. Fixed-layout BG scalars keep their
direct read path.

`In<TSF>::value_view()` borrows persistent series storage through its scalar
adapter. Returning that view from a helper taking `In` by value retains the
source lifetime. The adapter is used explicitly for borrowing. The owning
snapshot runtime is registered as the default for ordinary scalar storage.

Canonical `FrameSnapshotStorage` contains one owner pointer. The owner retains
the actual schema, runtime, dense column snapshots, row count, and allocation
owner. This fixed-size representation lets `TS<Frame<Base>>` retain all columns
of an assigned Derived frame. Assignment constructs the replacement owner
before releasing the previous owner. Borrowing a stored snapshot returns the
owner's actual runtime.

Optional extension-facing scalar storage trait operations:

```cpp
static value_type read_value(const details::ScalarData &data);
static const ScalarRuntimeType &runtime_type_for_copy(const ScalarData &source);
static const ScalarRuntimeType &runtime_type(const ScalarType &type);
static ScalarData borrow_value(const value_type &value);
```

The `read_value` operation constructs the declared view from the source representation.
The copy operation selects the runtime used for an owning copy. An owning
`ScalarValue<Frame<Base>>` copied from Derived selects the native snapshot runtime
for the actual Derived schema. Typed Arrow copies also select native snapshots;
erased Arrow copies retain their Arrow runtime. Node code uses the scalar and
time series APIs rather than these trait operations.

Canonical scalar runtime customization and borrowing of C++ views carrying
their actual semantic type both use `scalar_storage_traits<T>`.
`docs/scalar-api.md` defines their contracts. Erased owning copies use
`ScalarRuntimeType::runtime_type_for_copy(const ScalarData&) const`; its default
preserves the source runtime. A mutable frame borrow selects immutable snapshot
storage that retains its actual schema. Typed trait selection takes priority.

## Node and value APIs

Keep the existing concrete-schema surface: typed columns, `Row<Quotes>`,
`RowPatch<Quotes>`, zero-copy `value_view()`, and typed `In`/`Out` operations.
`FrameView<Quotes>` exposes its typed columns through read-only `columns()`,
matching `Frame<Quotes>`. This keeps column bindings consistent with the
retained actual schema.

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
A C++ schema field declared as `Field<SCALAR>` exposes a generic column through
`value.columns().field`; `view<T>()` checks its resolved cell type.

Generic outputs accept typed `Row<Schema>` for insertion and
`RowPatch<Schema>` for insertion and modification. Their argument adapters validate against
the resolved output fields and call the shared native mutation implementation.
Complete rows contain every output field. Patches distinguish omitted fields
from null cells. Insertion sets omitted patch fields to null; modification
leaves them unchanged. All current frame columns allow nulls.

Row/RowPatch fields declared with scalar variables store borrowed `Scalar<>`
handles. Concrete fields store nullable typed values. Adapters validate source
field declarations through BG type matching and check each present handle
against its resolved target cell type before mutation. Temporary numeric
storage decodes generic fields; native table storage retains no handles.

Projection binding accepts source ordinals in target-schema order. Named and
compile-time conveniences resolve names and call the same binding operation.

Live filtering belongs to a node. The expression implementation evaluates
blocks and supplies row-selection changes to its TSF view output. Expression
wiring uses BG materialisation options. The existing numeric filter helper is
a probe; the generic foundation does not add a cell callback filter API.

Arrow import gains a schema-inferred form and a form taking an explicit frame
schema descriptor. Both construct the same frame semantic types. Typed imports
validate against their C++ schema descriptor. Borrowed export and owning copy
continue to use the existing lifetime contracts.

## Extension boundary

Frame semantic types, field validation, storage, column operations, Arrow
adapters, and output options belong to the extension. The common BG descriptor
provides schema identity and field metadata. Report any required BG change for
owner review before implementing it.

Ordinary typed TS outputs assign through their configured canonical runtime;
they do not invoke the owning-copy selector. Frame's owner pointer preserves
actual-schema values at this boundary. Assignment from an immutable native
snapshot retains its owner. Assignment from a mutable TSF borrow copies current
cells into an independent snapshot.

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
