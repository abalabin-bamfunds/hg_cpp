# Borrowed literal input prototype

Configure and build from this lane's root. The engine target and build output
are local to this lane; the dependency source paths may point to existing
read-only dependency checkouts:

```sh
cmake -S prototypes/literal-input -B build/prototypes/literal-input -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_TPACK=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src \
  -DFETCHCONTENT_SOURCE_DIR_DDVISITOR=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src
cmake --build build/prototypes/literal-input --target literal_input_probe -j2
build/prototypes/literal-input/literal_input_probe
```

The probe covers scalar visitor dispatch, named native struct and Bundle
construction, over-aligned native fields, provider-backed Bundle fields,
recursive list, set, and dictionary construction, and rollback after
construction failures. It also checks nested Bundles constructed with a
different allocator while an outer scalar allocation context is active.

The prototype separates a borrowed scalar literal item from recursive literal
storage. `ScalarLiteralItem` stores a source address and a typed visit function
pointer. It canonicalizes raw C++ values during dispatch, then calls the existing
`details::make_scalar_source` utility while the canonical value remains alive.
`Scalar`, `ScalarValue`, and `ScalarData` use direct descriptor access.
`ScalarLiteralItem` borrows its source, which must remain alive through
dispatch. A descriptor passed to `Scalar<>::from(const ScalarData&)` must be
non-empty, and storage referenced by the descriptor must remain valid while the
resulting scalar view is used. A typed scalar view created during visitor
dispatch is callback-scoped and must not be retained.

`Literal<Leaf>` stores either one leaf or borrowed pointer-and-size sequence
storage. Sequence visitation converts it to `std::span<const Literal<Leaf>>`.
Sequence storage refers to initializer-list backing storage.
`ScalarLiteral` aliases `Literal<ScalarLiteralItem>` for recursive scalar
input. Construct, traverse, and consume a literal within the full expression
that created its initializer lists and temporary scalar sources. Do not retain
it beyond that expression.

`visit()` constructs a DDV adapter for one dispatch. `accept()` reuses a
caller-owned scalar visitor mux:

```cpp
auto visitor = ddv::make_visitable_visitor<bg::ScalarVisitTypes>(
    std::tuple<>{}, [](const bg::Scalar<bg::i64> *value) { (void)value->value(); }, ddv::noop);
auto &scalar_types = static_cast<bg::ScalarVisitTypes &>(visitor);
item.accept(scalar_types);
```

`construct<T>(literal)` builds a typed owning scalar in its final storage. The
runtime-only overload takes the destination runtime type. Both use the engine's
borrowed construction callbacks and recursively initialize lists, sets,
dictionaries, compound scalars, and bundles:

```cpp
auto nested = literal::construct<bg::List<bg::List<bg::i64>>>(
    {{1, 2}, {}, {3}});

auto dictionary = literal::construct<bg::Dict<bg::string, bg::List<bg::i64>>>(
    {{"a", {1, 2}}, {"b", {3, 4}}});

auto set = literal::construct<bg::Set<bg::i64>>({1, 2, 2});

struct Point {
    bg::i64 x;
    const bg::i64 y;
};
auto point = literal::construct<Point>({{"y", 2}, {"x", 1}});

template <template <typename, auto...> typename Field> struct Record {
    Field<bg::List<bg::i64>> values;
    Field<bg::i64, bg::optional> note;
};
auto memory = bg::allocator{};
auto record = literal::construct<bg::Bundle<Record>>(
    {{"values", {1, 2}}}, memory);

const auto &item_runtime = bg::cpp_scalar_runtime_type<bg::i64>();
const auto &inner_type = bg::list_scalar_type(item_runtime.type());
const auto inner_available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
const auto &inner_runtime = bg::resolve_scalar_runtime_type(inner_type, inner_available);
const auto &outer_type = bg::list_scalar_type(inner_type);
const auto outer_available = std::array<const bg::ScalarRuntimeType *, 1>{&inner_runtime};
const auto &runtime_type = bg::resolve_scalar_runtime_type(outer_type, outer_available);
auto erased = literal::construct(runtime_type, {{1, 2}, {}, {3}});
```

Construction dispatches each scalar leaf through `ScalarRuntimeType::construct_from_input`.
List runtimes acquire item storage and commit each item only after its callback
returns successfully. A thrown callback leaves the destination slot unconstructed;
the enclosing list destroys all previously committed items.

A dictionary sequence contains entries with exactly two literals: a key and
a mapped value. Each literal is interpreted recursively by its destination
runtime. Dictionary construction rejects duplicate keys with
`std::invalid_argument`. The dictionary runtime constructs each key in its
final slot before hashing and duplicate lookup. A duplicate destroys the
candidate key and skips mapped construction. Any failed entry destroys the
enclosing dictionary and its committed entries.

A set sequence contains item literals and deduplicates equal destination
values. Each item is constructed in its final slot before hashing; a duplicate
candidate is destroyed.

Compound scalar and bundle sequences contain named pairs. Names are strings
or symbols, and entry order is independent of schema order. Unknown names,
duplicate names, malformed pairs, and missing required fields throw
`std::invalid_argument`. Omitted optional bundle fields remain absent. An
empty sequence constructs an empty bundle or a bundle with only optional
fields.

`ScalarFieldConstruction` lets the runtime own field layout and cleanup.
Native compounds stage trivial fields in fixed stack storage and initialize
the aggregate once, including const fields. Bundles construct fields directly
in their shared control block. Nested bundles share the allocation owner, so
a child copied out of a bundle can outlive its parent. Creating a bundle
sequence outside a scalar allocation context requires an explicit allocator;
copying an existing bundle leaf uses its existing shared ownership. An
explicit allocator also applies to bundles nested inside lists, sets, and
dictionaries. Its memory resource must outlive the resulting values.

`ScalarVisitTypes` dispatches built-in atomic types to typed scalar views and
structural types to their generic views. Other declarations reach the
`Scalar<>` extension callback, where the consumer owns extension behavior,
optionally through `ScalarDispatch`; see [the scalar API](../../docs/scalar-api.md#optional-operations).

The probe reports object layout and counts global `operator new` calls for
source construction, first visit, warmed visit, and recursive construction
and traversal. It does not count allocations made directly through the engine
allocator or C allocation functions. Construction, warmed visit, and recursive
construction/traversal counts are asserted to be zero. The cold first-visit
count is reported separately because runtime registration may allocate.

With GCC 14.3, the measured sizes and counts were:

```text
ScalarLiteralItem: 16 bytes, alignment 8
ScalarLiteral: 24 bytes, alignment 8
operator new calls: first visit=0, source construction=0, warmed visit=0,
                    recursive construction/traversal=0
```

The probe, provider fixture, and their header code can be checked with ASan and
UBSan while linking the normal, lane-local engine library. This instruments
the probe and provider fixture, not the engine library:

```sh
c++ -std=c++23 -fsanitize=address,undefined -fsanitize-address-use-after-scope \
  -fno-omit-frame-pointer \
  -Iprototypes/literal-input/include -Iinclude \
  -Itests/value/scalar \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src/include \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src/include \
  prototypes/literal-input/probe.cpp tests/value/scalar/test_scalar_provider.cpp \
  -Lbuild/prototypes/literal-input/bg \
  -Wl,-rpath,$PWD/build/prototypes/literal-input/bg -lbg \
  -o build/prototypes/literal-input/literal_input_probe_asan
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_stack_use_after_scope=1:detect_leaks=0 \
  UBSAN_OPTIONS=print_stacktrace=1 build/prototypes/literal-input/literal_input_probe_asan
```

Leak detection is disabled because LeakSanitizer aborts in this environment.
