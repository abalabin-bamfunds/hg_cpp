# Arithmetic, comparison, and conversion

Use numeric and Boolean operators to construct calculations over fixed scalars or
changing values. C++ includes `<hg/library.hpp>` and imports `hg::operators`; Python
imports names from `hgraph.operators`.

## Arithmetic

To express formulas and comparisons that update with their inputs, use the built-in
arithmetic, comparison, and Boolean operators below. Operands can be scalars or time
series, but numeric types must match exactly; an integer does not convert to double
to match an overload.

| Operators | Supported scalar types |
| --- | --- |
| `add` | `i64`, `double`, `timedelta_t`, `string`; timestamp plus duration in either order |
| `sub` | `i64`, `double`, `timedelta_t`; timestamp minus duration or timestamp |
| `mul` | `i64`, `double`; `timedelta_t` by `i64` or `double` in either order |
| `div` | `i64`, `double` to `double`; `timedelta_t` by `i64` or `double` to `timedelta_t`; duration ratio to `double` |
| `divmod` | Matching `i64` or `double` operands; returns a two-item TSL |
| `floordiv` | `i64` to `i64`, `double` to `double`, duration ratio to `i64` |
| `lshift`, `rshift` | `i64` |
| `ln` | `double`; input must be positive |
| `mod` | `i64` to `i64`, `double` to `double` |
| `pow` | `i64` to `i64`, `double` to `double` |
| `round` | `double`, with an optional decimal digit count; result is `double` |
| `sign` | `i64`, `double` |
| `neg`, `pos`, `abs` | `i64`, `double`, `timedelta_t` |
| `bit_and`, `bit_or`, `bit_xor`, `invert` | `i64` |
| `eq`, `ne` | Compatible values of an equality-comparable scalar type |
| `lt`, `le`, `gt`, `ge` | `i64`, `double`, `string`, `date_time_t`, `timedelta_t` |
| `and_`, `or_`, `not_` | `bool` |

Generic `eq` and `ne` overloads accept two time series of the same equality-comparable
scalar type. Scalar-only and mixed scalar/time-series calls for compound, collection,
and extension types are not yet supplied.

Integer arithmetic wraps at `i64` width. Negation and absolute value of the minimum
integer return that same minimum value. Floating-point arithmetic uses native C++
operations. String operations process bytes, including embedded NUL bytes.
[Timestamp and duration operations](library-temporal.md) describe temporal arithmetic
and field access.

### Division and powers

To divide quantities while retaining a fractional result, use `/` or `div`.
Integer division converts both operands to `double`; large integers can round during
conversion, so results can differ from Python integer true division. Division preserves
the sign of a zero result. Use `floordiv` when the result needs the floor quotient,
and `mod` when only the remainder is needed. Division, floor division, and modulo
reject a zero divisor during evaluation.

Dividing the `i64` minimum by `-1` returns the positive quotient as `double`;
integer floor division wraps that quotient, and modulo returns zero. Integer `pow`
wraps at `i64` width and rejects negative exponents. Floating `pow` rejects zero
raised to a negative exponent and negative bases with fractional exponents.

### Quotient and remainder

If a calculation needs both complete units and a leftover amount, `divmod` produces
both from the same operands. Its fixed two-item time series list contains the floor
quotient at index 0 and the remainder at index 1. For example, dividing a quantity
into packs of three gives:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, TSL, graph, run_graph
from hgraph.operators import divmod

@graph
def packs(quantity: int) -> TSL[TS[int], 2]:
    return divmod(quantity, 3)

print(run_graph(packs, [7, 10, -7]))
```

Output:

```text
[{0: 2, 1: 1}, {0: 3, 1: 1}, {0: -3, 1: 2}]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;

int main() {
    std::cout << run_graph([](auto quantity) {
        return divmod(quantity, 3);
    }, {7, 10, -7}) << '\n';
}
```

Output:

```text
[{0: 2, 1: 1}, {0: 3, 1: 1}, {0: -3, 1: 2}]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Both operands must have the same numeric type. Each result item retains its resolved
guarantee; the list root is ordinary. Indexing the result selects one item as a
connection, as with other [time series lists](../guide/advanced-graphs/collections.md).

### Rounding

When a calculation needs a fixed decimal precision, use `round(value, ndigits)`.
The digit count is optional, and the result remains `double`. Rounding uses ties to
even and preserves non-finite values and the sign of zero. With a digit count it
scales by a power of ten, rounds, and scales back; floating-point scaling affects
some halfway values, so `round(2.675, 2)` returns `2.68`.

### Logarithms and signs

Use `ln` for natural logarithms, for example when converting ratios into log returns.
Use `sign` when only a value's direction matters, independently of its magnitude:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from math import e
from hgraph import graph, run_graph
from hgraph.operators import ln, sign

@graph
def log_return(ratio: float) -> float:
    return ln(ratio)

@graph
def direction(change: int) -> int:
    return sign(change)

print(run_graph(log_return, [1.0, e]))
print(run_graph(direction, [-20, 0, 35]))
```

Output:

```text
[0.0, 1.0]
[-1, 0, 1]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

#include <numbers>

using namespace hg;
using namespace hg::operators;

int main() {
    std::cout << run_graph([](auto ratio) {
        return ln(ratio);
    }, {1.0, std::numbers::e}) << '\n';
    std::cout << run_graph([](auto change) {
        return sign(change);
    }, {-20, 0, 35}) << '\n';
}
```

Output:

```text
[0, 1]
[-1, 0, 1]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

`ln` accepts `double`; a nonpositive value raises `domain_error` during evaluation.
`sign` returns -1, 0, or 1 in the input's numeric type. For `double`, either signed
zero produces positive zero and a NaN is returned unchanged.

### Integer bitwise operators

To combine integer flags or masks, use `bit_and`, `bit_or`, and `bit_xor` on the
64-bit representations of two `i64` values. `invert` complements every bit. These operators accept scalar or time-series
operands and do not convert `double` values. Their results retain the `i64` type.

C++ wiring points support `&`, `|`, `^`, and `~`. Python graph values support the same
syntax for `i64`. The named operators are also available. The existing collection
overloads for `bit_and`, `bit_or`, and `bit_xor` operate on TSS and TSD values; see
[set operations](library-collections.md#time-series-set-operations).

Extract the low three bits of each integer:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import bit_and

@graph
def low_bits(value: int) -> int:
    return bit_and(value, 7)

print(run_graph(low_bits, [9, 15, None, 16]))
```

Output:

```text
[1, 7, None, 0]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

int main() {
    std::cout << run_graph([](auto value) {
        return bit_and(value, 7);
    }, {9, 15, SKIP, 16}) << '\n';
}
```

Output:

```text
[1, 7, None, 0]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

### Bit shifts

To position bits in an integer mask or extract a shifted value, use `<<` and `>>`,
or their named forms `lshift` and `rshift`. Right shift preserves a negative value's
sign:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph

@graph
def shifted(value: int) -> int:
    return value >> 1

print(run_graph(shifted, [-7, 7, 16]))
```

Output:

```text
[-4, 3, 8]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;

int main() {
    std::cout << run_graph([](auto value) {
        return value >> 1;
    }, {-7, 7, 16}) << '\n';
}
```

Output:

```text
[-4, 3, 8]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Operands are `i64`. Shift counts must be nonnegative; a negative count raises
`domain_error` during evaluation. A left shift by 64 or more returns zero. A right
shift by 64 or more returns zero for a nonnegative value and -1 for a negative value.

### convert

If a calculation needs a different numeric type, use `convert` with the full
result time series type:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from hgraph import TS
from hgraph.operators import convert

convert[TS[float]](integer_value)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
convert[type<TS<double>>](integer_value);
```
:::
::::

The built-in overloads convert between `i64` and `double`. Converting `double` to `i64`
truncates toward zero; NaN raises `domain_error`, and infinities or values outside the
`i64` range raise `overflow_error`. The public operator accepts overloads for other
source time-series types, including collections; each overload's output must be
compatible with the requested result type. The requested type checks compatibility and
does not replace the overload's inferred output guarantee.

Required operands gate evaluation. Their invalidation invalidates the result, and
revalidation permits another calculation. Equal results from ordinary inputs still
produce ticks. Required calculations propagate the weakest observed input guarantee;
output publication enforces it. Reference inputs follow the [reference guarantee
rules](types.md#references).

Scalar-only calls evaluate once and infer a `const` output. Scalar-only string
concatenation calculates during construction.

## Optional arithmetic

If arithmetic should still produce a result when an operand is unavailable, use
`opt()` to select an optional overload. Addition substitutes zero, or the empty string, for
invalid operands. Multiplication substitutes one. These overloads accept two optional
operands and produce ordinary outputs. Marking either operand optional selects that
behavior for both operands.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from hgraph import opt
from hgraph.operators import default_

total = opt(x) + opt(y)
product = opt(x) * opt(y)
with_fallback = default_(x, 10.0) + y
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto total = opt(x) + opt(y);
auto product = opt(x) * opt(y);
auto with_fallback = default_(x, 10.0) + y;
```
:::
::::

Time series calls do not schedule on start. An input change can calculate a result while
either operand is invalid. No input change means no calculation. `default_()` supplies a
caller-selected fallback; it does not select optional arithmetic. Other arithmetic
operators reject `opt()` arguments.

## Wiring syntax

To write graph formulas with familiar expression syntax, use the overloaded operators
on wiring points. C++ supports binary `+`, `-`, `*`, `/`, `%`, `|`, `&`, `^`, `<<`, `>>`, unary `+`,
`-`, and `~`, and `==`, `!=`, `<`, `<=`, `>`, `>=`. Use `floordiv()`, `pow()`, `round()`, and
`abs()` for operations without C++ wiring syntax. These operations also accept scalar
operands and return `wp<>`.

For Boolean graph operations, C++ `!`, `&&`, and `||` call `not_`, `and_`, and `or_`.
Named calls are also available:

```cpp
auto bounded = (x > i64{0}) && (x < i64{10});
auto negated = !bounded;
```

Python uses `/`, `//`, `%`, and `**` for division, floor division, modulo, and power.
It uses `&`, `|`, and `^` for integer bitwise operators and collection set operations,
and `~` for integer bitwise inversion. `<<` and `>>` shift integer values. `round(point, ndigits)` and `abs(point)` call
their corresponding operators. Python uses the named Boolean operators because its
`and`, `or`, and `not` keywords cannot be overloaded. Write two comparisons explicitly:

```python
from hgraph.operators import and_, not_

bounded = and_(x > 0, x < 10)
negated = not_(bounded)
```

Boolean graph operations do not short-circuit. C++ evaluates both operand expressions
during wiring. Every time series operand must be valid before the node evaluates.

C++ `wp<T>` and `wp<>` do not convert to native `bool`. Python truth conversion raises
`TypeError`. Test output presence with `point.has_output()` in C++ or `point.has_output`
in Python.

See [operator declarations](operators.md) for selection and extension.

## clip

To keep a numeric result within allowed limits, use `clip(value, min_, max_)` to
clamp an integer or floating-point value to inclusive bounds.
All three arguments have the same numeric type and can be scalars or time series.
Required inputs must be valid. `min_ > max_` raises an evaluation error. The output
propagates the weakest observed input guarantee.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import clip

@graph
def limited(value: int) -> int:
    return clip(value, 0, 100)

print(run_graph(limited, [-2, 50, 120]))
```

Output:

```text
[0, 50, 100]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

int main() {
    const auto result = run_graph([](auto value) {
        return clip(value, 0, 100);
    }, {-2, 50, 120});
    std::cout << result << '\n';
}
```

Output:

```text
[0, 50, 100]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::
