# Arithmetic, comparison, and conversion

Use numeric and Boolean operators to construct calculations over fixed scalars or
changing values. C++ includes `<bg/library.hpp>` and imports `bg::operators`; Python
imports names from `bg.operators`. The fragments below use existing connections `x`,
`y`, and `integer_value`.

## Arithmetic

Built-in operands can be scalars or time series. Numeric operands have exact types; an
integer operand does not convert to double to match an overload.

| Operators | Supported scalar types |
| --- | --- |
| `add` | `i64`, `double`, `timedelta_t`, `string`; timestamp plus duration in either order |
| `sub` | `i64`, `double`, `timedelta_t`; timestamp minus duration or timestamp |
| `mul` | `i64`, `double` |
| `div` | `i64`, `double`; result is `double` |
| `floordiv` | `i64` to `i64`, `double` to `double` |
| `mod` | `i64` to `i64`, `double` to `double` |
| `pow` | `i64` to `i64`, `double` to `double` |
| `round` | `double`, with an optional decimal digit count; result is `double` |
| `neg`, `pos`, `abs` | `i64`, `double`, `timedelta_t` |
| `eq`, `ne` | Compatible values of an equality-comparable scalar type |
| `lt`, `le`, `gt`, `ge` | `i64`, `double`, `string`, `date_time_t`, `timedelta_t` |
| `and_`, `or_`, `not_` | `bool` |

Generic `eq` and `ne` overloads accept two time series of the same equality-comparable
scalar type. Scalar-only and mixed scalar/time-series calls for compound, collection,
and extension types are not yet supplied.

Timestamp subtraction produces a duration. Timestamp minus duration produces a
timestamp. Integer and temporal arithmetic wraps at the underlying integer width.
Negation and absolute value of the minimum integer or duration return that same minimum
value. Floating-point arithmetic uses normal C++ operations. String operations process
bytes, including embedded NUL bytes. Temporal operators are native C++ overloads. Python
`datetime.timedelta` maps to `timedelta_t`; Python timestamp scalar conversion is not
yet supplied.

Integer division converts both operands to `double` and uses native floating-point
division. Conversion can round operands outside the consecutive-integer range, so
large-integer results can differ from Python integer true division. It preserves the
sign of a zero result. Division, floor division, and modulo reject a zero divisor during
evaluation. Dividing the `i64` minimum by `-1` returns the positive quotient as
`double`; integer floor division wraps the corresponding quotient, and modulo returns
zero. Integer power wraps at `i64` width and rejects negative exponents. Floating power
rejects zero raised to a negative exponent and negative bases with fractional exponents.
`round` returns `double` and rounds ties to even. With a digit count, it scales the
value by a power of ten, rounds, and scales back. Floating-point scaling can affect the
result: `round(2.675, 2)` returns `2.68`. It preserves non-finite values and the sign of
zero.

### convert

`convert` requires an explicit full result time-series type:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import TS
from bg.operators import convert

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

`opt()` selects optional overloads. Addition substitutes zero, or the empty string, for
invalid operands. Multiplication substitutes one. These overloads accept two optional
operands and produce ordinary outputs. Marking either operand optional selects that
behavior for both operands.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import opt
from bg.operators import default_

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

C++ wiring points support binary `+`, `-`, `*`, `/`, `%`, `|`, `&`, `^`, unary `+` and
`-`, and `==`, `!=`, `<`, `<=`, `>`, `>=`. Use `floordiv()`, `pow()`, `round()`, and
`abs()` for operations without C++ wiring syntax. These operations also accept scalar
operands and return `wp<>`.

For Boolean graph operations, C++ `!`, `&&`, and `||` call `not_`, `and_`, and `or_`.
Named calls are also available:

```cpp
auto bounded = (x > i64{0}) && (x < i64{10});
auto negated = !bounded;
```

Python uses `/`, `//`, `%`, and `**` for division, floor division, modulo, and power.
`round(point, ndigits)` and `abs(point)` call their corresponding operators. Python uses
the named Boolean operators because its `and`, `or`, and `not` keywords cannot be
overloaded. Write two comparisons explicitly:

```python
from bg.operators import and_, not_

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

`clip(value, min_, max_)` clamps an integer or floating-point value to inclusive bounds.
All three arguments have the same numeric type and can be scalars or time series.
Required inputs must be valid. `min_ > max_` raises an evaluation error. The output
propagates the weakest observed input guarantee.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import clip

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
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto limited(wp<TS<i64>> value) {
    return clip(value, i64{0}, i64{100});
}

int main() {
    const auto result = run_graph(limited, {-2, 50, 120});
    std::cout << std::boolalpha << result.equals({0, 50, 100}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::
