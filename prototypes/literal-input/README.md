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
pointer. It
canonicalizes raw C++ values only during `visit()`, then calls the existing
`details::make_scalar_source` utility while the canonical value remains alive.
`Scalar`, `ScalarValue`, and `ScalarData` use direct descriptor access.
`ScalarLiteralItem` borrows its source, which must remain alive through `visit()`. A
`ScalarData` descriptor passed to the callback is valid only until the callback
returns.

`Literal<Leaf>` stores either one leaf or a span of child elements. The span
refers to initializer-list backing storage. `ScalarLiteral` aliases
`Literal<ScalarLiteralItem>` for recursive scalar input. Construct, traverse,
and consume a literal within the full expression that created its initializer
lists and temporary scalar sources. Do not retain it beyond that expression.

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

The probe and its headers can be checked with ASan and UBSan while linking the
normal, lane-local engine library. This instruments the probe and header code,
not the engine library:

```sh
c++ -std=c++23 -fsanitize=address,undefined -fsanitize-address-use-after-scope \
  -fno-omit-frame-pointer \
  -Iprototypes/literal-input/include -Iinclude \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/ddvisitor-src/include \
  -I/home/abalabin/_bg/.lanes/literal-implementation/build/dev/_deps/tpack-src/include \
  prototypes/literal-input/probe.cpp -Lbuild/prototypes/literal-input/bg \
  -Wl,-rpath,$PWD/build/prototypes/literal-input/bg -lbg \
  -o build/prototypes/literal-input/literal_input_probe_asan
ASAN_OPTIONS=detect_stack_use_after_return=1:detect_stack_use_after_scope=1:detect_leaks=0 \
  UBSAN_OPTIONS=print_stacktrace=1 build/prototypes/literal-input/literal_input_probe_asan
```

Leak detection is disabled because LeakSanitizer aborts in this environment.
