# Frame schema foundation

Status: proposal for owner review. No schema implementation is authorized by
this document. Names below describe proposed APIs.

## Purpose

Use one frame implementation for concrete C++ schemas, schema variables, and
schemas constructed programmatically. Generic nodes must preserve a captured
schema through ordinary BG type resolution. Native and Arrow representations
must expose the same value and change APIs.

The existing source is preserved in `frame-output-options`. This lane starts
from BG `e470d432`. All five existing extension tests pass against an installed
BG package from that revision with Arrow enabled.

The [shared BG schema proposal](../../../schema-foundation/experiments/schema_foundation/proposal.md)
now precedes frame adaptation. Frames use its common descriptor and factories.
The foundation is implemented and verified in its lane. The existing frame
extension also passes its five tests against that installed BG package with
Arrow enabled, without source changes.

## Scope to confirm

A schema supplied programmatically during wiring is resolved and fixed for
the graph lifetime. Evaluation-time changes to the column set are a separate
contract. The earlier dictionary-of-whole-columns proposal remains the
candidate representation for pivot columns.

The initial cell implementations remain nullable `i64` and `double`.
Unsupported resolved cell types fail during wiring. This work adds schema
generality rather than additional cell layouts or expression operations.

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

## Required BG change for review

Before the shared foundation, BG's `SCHEMA` variable was specifically
`TypeVariableType<BundleSchemaType>`. Its resolver parameters and diagnostics
also assumed that category. A frame schema could not use this variable without
pretending to be a bundle schema or inventing a second variable identity.

The approved shared BG schema proposal supplies `SchemaType : Type` and the
common `Schema` descriptor, migrating both bundle and hierarchical scalar
metadata:

- TSB and hierarchical scalar types refer to the common `Schema` descriptor.
- `SCHEMA` captures `SchemaType`; the declaration supports named variables.
- Schema variable parameters, mapping validation, and overload specificity
  use this category.
- Bundle resolution checks time series field types; frame resolution checks
  cell scalar field types and column presence.
- `schema_type<SCHEMA>` remains the resolver parameter declaration. Concrete
  bundle schema declarations retain their current meaning.

The common descriptor owns no frame storage, column operations, or Arrow
semantics. Frame semantic types, access, runtimes, and output options remain
in the extension. No implementation is to be made before owner review.

## Build order and acceptance

1. Verify the existing numeric extension baseline against latest BG (complete).
2. Implement the approved common schema foundation in BG (complete on its lane).
3. Add canonical frame/column semantic descriptors and generic resolution.
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
