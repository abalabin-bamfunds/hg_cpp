# Shared schema foundation

Status: descriptor, identity key, and projected field interpretation approved.
Implementation and verification are complete in the schema foundation lane.
Main integration is pending. Generic scalar bundles precede frame adaptation.

## Purpose

Centralize runtime schema metadata and its construction. Hierarchical scalar
types, time series bundle types, and extension types should use the same
descriptor implementation. Retain each family's type matching, runtime
layout, and field interpretation.

The current shared C++ `schema_reflection` and `schema_extends` APIs remain
the source of named declarations.

## Common descriptor

```cpp
struct SchemaField {
    util::symbol name;
    const Type* type;
    bool optional;
};

class SchemaType : public Type {
    // Category used by schema variables.
};

class Schema : public SchemaType {
    // Named identity, or anonymous structural identity.
    // Base schema, direct fields, flattened ordered fields.
    // Name lookup, declaration ancestry, resolution.
};
```

The descriptor stores a name, a declaration identity, a `const Schema*` base
pointer, and owned direct/flattened field arrays. Its canonical factory uses
this exact key:

```cpp
using SchemaIdentity =
    std::variant<std::monostate, std::type_index, RuntimeSchemaId>;

struct SchemaKey {
    util::symbol name;
    SchemaIdentity identity;
    const Schema* base;
    std::vector<SchemaField> direct_fields;
};
```

For a C++ schema template `Quote`, the declaration token is
`std::type_index{typeid(details::schema_template<Quote>)}`. This uses the
existing schema-template wrapper in `schema/reflection.hpp`; no value object
or fields are instantiated to construct the token. Programmatic anonymous
schemas use `std::monostate`.

BG allocates a unique `RuntimeSchemaId` for each runtime declaration. Python
schema construction uses this ID to preserve nominal identity without retaining
a Python object. A declaration reuses its ID across projections. Separately
declared runtime schemas remain distinct even when their names and fields match.

Key equality compares the name, declaration token, base pointer, and each
ordered direct field's name, canonical semantic type pointer, and optional
bit. Flattened fields are derived from the base and direct fields. A cache
hit returns the same registered `Schema` object. The cache hashes these same
key components.

The hierarchical scalar semantic-type cache retains its existing
`std::type_index{typeid(Bundle<Quote>)}` key. The schema descriptor's template
token is independent of the semantic type that uses the schema. TSB's previous
inline-marker address becomes the shared schema-template token; named ancestry
continues to compare declaration tokens along the base chain.

`SchemaField::type` records the actual semantic field type. A schema may have
unresolved fields while wiring. Resolved descriptors and schema variables are
registered types with process lifetime.

The common implementation provides:

- Canonical descriptor construction from an ordered field list, a name,
  optional named identity, and an optional base descriptor.
- Duplicate-name validation, inherited field ordering, and redefinition
  validation. A redefinition retains the inherited ordinal, accepts a
  compatible narrower type, and preserves required presence.
- `field_count`, `field`, `direct_fields`, `fields`, `find`, and declared
  ancestry lookup.
- Resolution of field variables and base descriptors through a BG type map.
- Named identity and anonymous structural identity. Resolved field types
  participate in the descriptor key, as they already do for TSB schemas.

Factories use hashed registry keys. Constructing and hashing an input field
list remains proportional to its length; locating a descriptor does not scan
every registered schema.

`optional` describes an absent field. It does not describe a nullable cell in
a frame column. Family construction validates supported field types before a
resolved semantic type or runtime can be selected.

## Family ownership

```text
HierarchicalScalarType : ScalarType {
    Schema& schema
    scalar capabilities
}

TimeSeriesBundleType : FixedItemTimeSeriesType {
    SchemaType& schema_or_variable
    guarantee
}

Future FrameType / FrameSeriesType {
    SchemaType& schema_or_variable
}
```

Hierarchical scalar schemas hold scalar field types and optional-field
presence. TSB schemas hold time series field types and structurally present
fields. Frame schemas will hold cell scalar types with present columns.

The owning family retains its compatibility rules. Hierarchical scalar
matching follows declared nominal inheritance. TSB matching retains its
existing named ancestry and field matching rules. Frame matching will be
settled in the frame proposal.

Runtime types continue to own layout, allocation, lifecycle, and field access.
The common descriptor contains no offsets, buffers, publication state, or
frame operations. Runtime construction validates the field family once and
binds the corresponding scalar or time series runtime types.

Existing per-value representations do not change during this migration.

## Schema interpretation

Retain the current interpretation of schema fields:

```text
Bundle<Quote> -> Schema fields { bid: double, ask: double }
TSB<Quote>    -> Schema fields { bid: TS<double>, ask: TS<double> }
```

Both descriptors use the same implementation and C++ reflection. Their
canonical keys include named identity and actual field metadata, so projected
field types produce distinct descriptor values. Users do not supply a second
identifier for the TSB projection. TSB guarantee normalization continues to
operate on its projected field types.

`SCHEMA` becomes a variable over the common schema category. It captures the
descriptor used by the supplied family. Sharing one variable between two
parameters requires the same captured descriptor; the variable does not
perform a scalar-to-time-series schema conversion.

## Public and extension APIs

- Keep concrete C++ declarations such as `Bundle<Quote>` and `TSB<Quote>`.
- Add `schema()` to `HierarchicalScalarType`; TSB already exposes it.
- Replace separate `HierarchicalScalarField` and `BundleField` metadata with
  the common descriptor fields. Scalar/time-series type access validates the
  family and then uses its known type category.
- Generalize `SCHEMA`, named schema variable declarations, variable instance
  registration, resolver parameters, and mapping checks to `SchemaType`.
- Keep the meaning of concrete `schema_type<Quote>` as the existing TSB
  projection for this migration. `schema_type<SCHEMA>` is family-neutral.
- Provide extension-facing factories for programmatic schemas. Graph/node
  authors use descriptors and typed/generic views, not registration internals.

Generic hierarchical scalar declarations such as `Bundle<SCHEMA>` require
additional declaration/access work. The shared descriptor enables that work;
it does not silently add a new matching contract in this migration.

## Recursive registration

Recursive hierarchical declarations reserve semantic types before collecting
their fields. The shared schema factory must use the same registration
transaction: schema entries and semantic types publish, commit, or roll back
as one group.

Do not publish a schema cache entry that refers to a provisional scalar type.
An ordinary cache's committed return-value contract cannot supply rollback
for such entries. Keep the existing recursive group transaction and make its
schema lifetime ownership explicit.

Required-field cycle validation, recursive capability calculation, and
cross-library declaration validation remain required gates. Canonical
descriptors expose immutable, complete fields after successful registration.

## Migration and verification

1. Add shared schema descriptors, registration, factories, and focused tests.
2. Migrate TSB descriptors and schema variables; retain existing wiring and
   guarantee behavior.
3. Migrate hierarchical scalar metadata and recursive registration; retain
   its nominal types and shared runtime storage.
4. Update schema diagnostics and authoritative documentation. Record the
   implemented durable decision in the same authorized commit.
5. Run focused schema/type tests and the full BG gate, including Python and
   installed-package verification.
6. Rebuild the existing frame extension against the resulting BG package
   before adapting its schema/storage implementation.

Tests must cover named/anonymous canonical identity, lookup/order, inherited
redefinitions, optional presence, generic resolution, repeated variables,
guarantees, recursive and mutually recursive scalar types, registration
failure cleanup, and shared-library identity. Existing evaluation tests verify
that migrating metadata does not change runtime behavior.
