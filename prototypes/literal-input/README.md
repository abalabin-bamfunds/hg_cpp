# Literal probes

The literal carriers live in `include/bg/literal/`. Scalar construction and
replacement live in `include/bg/scalar/literal.hpp`. See the
[literal API](../../docs/scalar-api.md#literals) for source lifetimes and scalar
construction, and the [node API](../../docs/node-api.md) for `Out::apply()`.

Configure and build from this lane's root. The engine target and build output
are local to this lane; the dependency source paths may point to existing
read-only dependency checkouts:

```sh
cmake -S prototypes/literal-input -B build/prototypes/literal-input -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_TPACK=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src \
  -DFETCHCONTENT_SOURCE_DIR_DDVISITOR=/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src
cmake --build build/prototypes/literal-input --target literal_input_probe -j2
build/prototypes/literal-input/literal_input_probe
cmake --build build/prototypes/literal-input --target literal_time_series_apply_probe -j2
build/prototypes/literal-input/literal_time_series_apply_probe
```

The probe covers scalar visitor dispatch, named native struct and Bundle
construction, over-aligned native fields, provider-backed Bundle fields,
recursive list, set, and dictionary construction, and rollback after
construction failures. It also checks nested Bundles constructed with a
different allocator while an outer scalar allocation context is active.

`visit()` constructs a DDV adapter for one dispatch. `accept()` reuses a
caller-owned scalar visitor mux:

```cpp
auto visitor = ddv::make_visitable_visitor<bg::ScalarVisitTypes>(
    std::tuple<>{}, [](const bg::Scalar<bg::i64> *value) { (void)value->value(); }, ddv::noop);
auto &scalar_types = static_cast<bg::ScalarVisitTypes &>(visitor);
item.accept(scalar_types);
```

`ScalarValue<T>::from_literal(literal)` builds a typed owning scalar in its final
storage. The erased overload takes the destination runtime type. Both use the engine's
borrowed construction callbacks and recursively initialize lists, sets,
dictionaries, compound scalars, and bundles:

```cpp
auto nested = bg::ScalarValue<bg::List<bg::List<bg::i64>>>::from_literal(
    {{1, 2}, {}, {3}});

auto dictionary = bg::ScalarValue<bg::Dict<bg::string, bg::List<bg::i64>>>::from_literal(
    {{"a", {1, 2}}, {"b", {3, 4}}});

auto set = bg::ScalarValue<bg::Set<bg::i64>>::from_literal({1, 2, 2});

struct Point {
    bg::i64 x;
    const bg::i64 y;
};
auto point = bg::ScalarValue<Point>::from_literal({{"y", 2}, {"x", 1}});

template <template <typename, auto...> typename Field> struct Record {
    Field<bg::List<bg::i64>> values;
    Field<bg::i64, bg::optional> note;
};
auto memory = bg::allocator{};
auto record = bg::ScalarValue<bg::Bundle<Record>>::from_literal(
    {{"values", {1, 2}}}, memory);

const auto &item_runtime = bg::cpp_scalar_runtime_type<bg::i64>();
const auto &inner_type = bg::list_scalar_type(item_runtime.type());
const auto inner_available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
const auto &inner_runtime = bg::resolve_scalar_runtime_type(inner_type, inner_available);
const auto &outer_type = bg::list_scalar_type(inner_type);
const auto outer_available = std::array<const bg::ScalarRuntimeType *, 1>{&inner_runtime};
const auto &runtime_type = bg::resolve_scalar_runtime_type(outer_type, outer_available);
auto erased = bg::ScalarValue<>::from_literal(runtime_type, {{1, 2}, {}, {3}});
```

Sequence syntax, field validation, allocator ownership, and failure cleanup
follow the [scalar API](../../docs/scalar-api.md#literals).

`ScalarVisitTypes` dispatches built-in atomic types to typed scalar views and
structural types to their generic views. Other declarations reach the
`Scalar<>` extension callback, where the consumer owns extension behavior,
optionally through `ScalarDispatch`; see [the scalar API](../../docs/scalar-api.md#optional-operations).

`time_series_apply.hpp` provides `literal::apply(output, literal)` for typed
and runtime-selected scalar time series and fixed TSL outputs:

```cpp
#include <bg/literal/scalar_literal.hpp>
#include "literal_input/time_series_apply.hpp"

literal::apply(scalar_output, 42);
literal::apply(list_output, {{0, 10}, {2, 30}});
literal::apply(nested_list_output, {{1, {{0, 10}}}});
```

Scalar TS outputs delegate to `Out::apply()`, which uses the scalar construction
consumer for replacement and publishes successful updates. Scalar leaves use
runtime assignment without an owning intermediate; scalar sequences construct
complete replacements through the actual destination runtime.
TSL entries are `{index, child_delta}` pairs. Indices must be nonnegative
integers other than Boolean values. At each list, the consumer validates entry
shape, index range, and duplicate indices before applying that list's children.
Validation uses an allocation-free scan of earlier entries. Omitted indices
retain their state. Empty deltas perform no updates. Child outputs own validity,
guarantees, notifications, and containing paths. Nested deltas apply recursively;
a child application failure can leave earlier children published, as defined
by [evaluation](../../docs/evaluation.md#exceptions).

Scalar time series accept replacement sequences for lists, sets, dictionaries,
compound scalars, and bundles. TSB, TSD, and TSS output updates remain unsupported.

The `literal_time_series_apply_probe` target checks typed and generic output
dispatch, nested TSL updates, notifications, validity, deduplicated outputs,
borrowed scalar views, provider-backed rvalue leaves, malformed deltas, and
unsupported output types. Scalar list checks cover native and composed runtimes,
three nested list levels, empty replacements, deduplicated revalidation, provider
items, and failed later-item construction preserving previous values and
timestamps. Native and composed replacements each construct two supplied items
with exactly two item copies and one collection allocation. The scalar storage
regression checks the default provider assignment fallback and its cleanup.
Retained owning copies of nested lists and shared strings preserve their values
across replacement, failed construction, and destruction of the source output
storage.

Scalar replacement checks also cover native and composed sets and dictionaries,
nested dictionary/list values, over-aligned compounds with const fields, and
bundles containing dictionaries and compounds. They verify retained owning copies,
failed replacement cleanup, and equality suppression. The default scalar provider
fallback is exercised with a shared bundle representation.

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

The time-series probe can also run under ASan and UBSan while linking the
normal lane-local engine library:

```sh
/home/abalabin/miniforge3/envs/bg-build/bin/c++ -std=c++23 \
  -fsanitize=address,undefined -fsanitize-address-use-after-scope \
  -fno-omit-frame-pointer \
  -Iprototypes/literal-input/include -Iinclude -Itests/value/scalar \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src/include \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src/include \
  prototypes/literal-input/time_series_apply_probe.cpp tests/value/scalar/test_scalar_provider.cpp \
  -Lbuild/prototypes/literal-input/bg \
  -Wl,-rpath,/home/abalabin/_bg/.lanes/literal-input-prototype/build/prototypes/literal-input/bg \
  -lbg -o build/prototypes/literal-input/literal_time_series_apply_probe_asan
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_stack_use_after_scope=1:detect_leaks=0 \
  UBSAN_OPTIONS=print_stacktrace=1 build/prototypes/literal-input/literal_time_series_apply_probe_asan
```

Leak detection is disabled because LeakSanitizer aborts in this environment.
