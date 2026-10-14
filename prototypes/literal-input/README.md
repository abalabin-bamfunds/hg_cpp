# Borrowed literal input prototype

Configure and build from this lane's root. The engine target and build output
are local to this lane; the dependency source paths may point to existing
read-only dependency checkouts:

```sh
cmake -S prototypes/literal-input -B build/prototypes/literal-input -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_TPACK=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src \
  -DFETCHCONTENT_SOURCE_DIR_DDVISITOR=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src
cmake --build build/prototypes/literal-input --target literal_input_probe
build/prototypes/literal-input/literal_input_probe
```

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

`Literal<Leaf>` stores either one leaf or a span of child elements. The span
refers to initializer-list backing storage. `ScalarLiteral` aliases
`Literal<ScalarLiteralItem>` for recursive scalar input. Construct, traverse,
and consume a literal within the full expression that created its initializer
lists and temporary scalar sources. Do not retain it beyond that expression.

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
borrowed `ScalarConstruction` callback and recursively initialize nested lists:

```cpp
auto nested = literal::construct<bg::List<bg::List<bg::i64>>>(
    {{1, 2}, {}, {3}});

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
