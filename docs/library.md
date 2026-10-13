# Standard library

## Public surface

C++ graph code includes `<bg/library.hpp>` and links `bg::library`. Python
graph code uses `bg.operators`. Both surfaces expose native library operators
and the engine's `const_`, `default_`, `getitem`, `getattr`, `map`, `mesh`, and
`switch_` operators. All use normal overload resolution.

```cpp
#include <bg/library.hpp>

using namespace bg;

const auto calculate = graph([](wp<TS<double>> x, wp<TS<double>> y) {
    return (x + 2.0) * y;
});
```

```python
import bg

@bg.graph
def calculate(x: bg.TS[float], y: bg.TS[float]) -> bg.TS[float]:
    return (x + 2.0) * y
```

## Scalar operators

Built-in operands can be scalars or time series. Numeric operands have exact
types; an integer operand does not convert to double to match an overload.

| Operators | Supported scalar types |
| --- | --- |
| `add` | `i64`, `double`, `timedelta_t`, `string`; timestamp plus duration in either order |
| `sub` | `i64`, `double`, `timedelta_t`; timestamp minus duration or timestamp |
| `mul` | `i64`, `double` |
| `neg`, `pos`, `abs` | `i64`, `double`, `timedelta_t` |
| `eq`, `ne` | Compatible values of an equality-comparable scalar type |
| `lt`, `le`, `gt`, `ge` | `i64`, `double`, `string`, `date_time_t`, `timedelta_t` |
| `and_`, `or_`, `not_` | `bool` |

Generic `eq` and `ne` overloads accept two time series of the same
equality-comparable scalar type. Scalar-only and mixed scalar/time-series
calls for compound, collection, and extension types are not yet supplied.

Timestamp subtraction produces a duration. Timestamp minus duration produces a
timestamp. Integer and temporal arithmetic wraps at the underlying integer
width. Negation and absolute value of the minimum integer or duration return
that same minimum value. Floating-point arithmetic uses normal C++ operations.
String operations process bytes, including embedded NUL bytes. Temporal
operators are native C++ overloads; Python temporal conversions are not yet
supplied.

Required operands gate evaluation. Their invalidation invalidates the result,
and revalidation permits another calculation. Equal results from ordinary
inputs still produce ticks. Required calculations propagate the weakest
observed input guarantee; output publication enforces it. Reference inputs
follow the [reference guarantee rules](type-system.md#reference-guarantees).

Scalar-only calls, except string concatenation, use the same node implementations
and evaluate once in the first cycle. Required calculations infer a `const`
output. Compilation retains the calculation node. Scalar-only string
concatenation currently calculates during wiring and returns `const_`.

## Optional arithmetic

`opt()` selects optional overloads. Addition substitutes zero, or the empty
string, for invalid operands. Multiplication substitutes one. These overloads
accept two optional operands and produce ordinary outputs. Marking either
operand optional selects that behavior for both operands.

```cpp
auto total = opt(x) + opt(y);
auto product = opt(x) * opt(y);
auto with_fallback = operators::default_(x, 10.0) + y;
```

Time series calls do not schedule on start. An input change can calculate a
result while either operand is invalid. No input change means no calculation.
`default_()` supplies a caller-selected fallback; it does not select optional
arithmetic. Other operators reject `opt()` arguments.

## Wiring syntax

C++ wiring points support binary `+`, `-`, `*`, unary `+` and `-`, and
`==`, `!=`, `<`, `<=`, `>`, `>=`. Use `operators::abs()` for absolute value.
These operations also accept scalar operands and return `wp<>`.

For Boolean graph operations, C++ `!`, `&&`, and `||` call `not_`, `and_`, and
`or_`. Named calls are also available:

```cpp
auto bounded = (x > i64{0}) && (x < i64{10});
auto negated = !bounded;
```

Python uses the named Boolean operators because its `and`, `or`, and `not`
keywords cannot be overloaded. It uses `abs(point)` for absolute value and
writes two comparisons explicitly:

```python
bounded = bg.operators.and_(x > 0, x < 10)
negated = bg.operators.not_(bounded)
```

Boolean graph operations do not short-circuit. C++ evaluates both operand
expressions during wiring. Every time series operand must be valid before the
node evaluates.

C++ `wp<T>` and `wp<>` do not convert to native `bool`. Python truth conversion
raises `TypeError`. Test output presence with `point.has_output()` in C++ or
`point.has_output` in Python.

See the [implementation guidelines](library-implementation.md) when adding an
overload, and the [operator contract](operators.md) for selection and extension.
