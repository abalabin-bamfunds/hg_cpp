# Text operations

Python imports text operators from `bg.operators`. C++ imports `bg::operators` from
`<bg/library.hpp>`. Text operands can be scalar strings or string time series.
Symbol operands are accepted by character and case operations; produced text is a
string. Required input invalidation invalidates the result. Operations propagate
the weakest observed input guarantee unless stated otherwise.

## Construction and queries

| Expression | Result |
| --- | --- |
| `left + right` | Concatenated text |
| `text * count` | Repeated text; nonpositive count produces an empty string |
| `len_(text)` | Unicode code-point count |
| `contains_(text, part)` | Substring membership; operands have the same scalar type |
| `text[index]` | One code point as a string; out-of-range access fails |
| `substr(text, start, end)` | Half-open code-point range; omitted end means through the end |
| `lower(text)`, `upper(text)`, `casefold(text)` | Unicode case mapping, independent of locale |
| `str_(value)` | Python-style representation of a supported built-in scalar or scalar collection |

Negative positions count from the end. Substring bounds clamp to the available
code points; reversed bounds produce empty text. Character operations reject
malformed UTF-8. Combining marks count separately. Text storage, concatenation,
and comparison preserve bytes, including embedded NUL bytes; they do not normalize
Unicode. Case folding also does not normalize Unicode.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import substr, upper

@graph
def prefix(name: str) -> str:
    return "sensor: " + upper(substr(name, 0, 2))

print(run_graph(prefix, ["north", "south"]))
```

Output:

```text
['sensor: NO', 'sensor: SO']
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

auto prefix(wp<TS<string>> name) {
    return "sensor: " + upper(substr(name, i64{0}, i64{2}));
}

int main() {
    const auto result = run_graph(prefix, {"north", "south"});
    std::cout << std::boolalpha << result.equals({"sensor: NO", "sensor: SO"}) << '\n';
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

## Formatting and printing

`format_(pattern, *values, **named)` uses `{fmt}` format specifications. The pattern
and formatting arguments must be valid. Scalar-only calls evaluate at graph start.
C++ named arguments use `"name"_n = value` with `bg::literals` imported.

`print_(pattern, *values, **named)` writes formatted text followed by a newline and
has no output. `__std_err__=True` selects standard error; this option is a fixed
Boolean. Print calls produce separate nodes even when their arguments match.

`print_(series)` prints a time-series tick. Formatting arguments can also be
collection time series, for example `print_("book={}", book)`. Collections show
changed entries, including `INVALID`, dictionary `REMOVE`, and set
`REMOVED(value)` markers. Collection ticks accept only an empty format
specification (`{}`). Valid scalar arguments retain specifications such as
`{:04d}`. Use `{}` for arguments that may print `SKIP` or `INVALID`; these markers
do not accept scalar format specifications.
When another argument triggers printing, an unchanged collection prints `SKIP`;
a valid scalar argument prints its current value.
Passing a string series alone prints its tick data, including quotes around
strings. A scalar string remains a format pattern. To format a string-series
pattern, supply at least one formatting argument.

`log_(series)` and `log_(pattern, *values, **named)` send the same tick data and
formatted messages to the process-level `bg` logger. The fixed string option
`__level__` selects severity and defaults to `"info"`.
Accepted values are `"trace"`, `"debug"`, `"info"`, `"warning"`, `"error"`,
`"critical"`, and `"off"`. Unknown levels are wiring errors.

```python
log_(book)
log_("book={}", book, __level__="debug")
```

```cpp
log_(book);
log_("book={}", book, "__level__"_n = "debug");
```

Disabled levels skip data capture. Enabled messages capture their data during
graph evaluation and format it on Quill's logging thread. Logging can finish
after graph execution returns. Backend formatting errors follow Quill's error
handling. Call `bg::logging::flush()` to wait for queued messages.

C++ applications configure logging at startup with `<bg/logging.hpp>`.
`initialize(sinks, pattern_options, backend_options)` accepts Quill's sink,
`PatternFormatterOptions`, and `BackendOptions` types. The options are optional;
an empty sink list selects stdout. The default pattern includes time, level,
logger name, message, and any MDC fields. Time uses local wall time with
microsecond precision. Thread ID and source location are omitted.

```cpp
#include <bg/logging.hpp>
#include <quill/Frontend.h>
#include <quill/sinks/FileSink.h>

auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>("graph.log");
auto* logger = bg::logging::initialize(
    {sink}, quill::PatternFormatterOptions{"%(time) %(log_level) %(message)", "%H:%M:%S.%Qus"});
logger->set_log_level(quill::LogLevel::Debug);
```

Call `initialize` before accessing BG's logger or starting Quill's backend.
Late or repeated initialization throws `std::logic_error`. Without explicit
initialization, BG selects stdout unless it adopts an existing `bg` logger.
Quill's default backend options apply when BG starts the backend.
`bg::logging::logger()` returns the same borrowed logger for runtime level
changes. Quill owns it; keep it registered while BG can use it.

Applications that already manage Quill can create a logger named `"bg"` through
Quill before accessing BG's logger. BG adopts that logger.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import format_

@graph
def label(value: int) -> str:
    return format_("sensor {:03}", value)

print(run_graph(label, [2, 12]))
```

Output:

```text
['sensor 002', 'sensor 012']
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

auto label(wp<TS<i64>> value) {
    return format_("sensor {:03}", value);
}

int main() {
    const auto result = run_graph(label, {2, 12});
    std::cout << std::boolalpha << result.equals({"sensor 002", "sensor 012"}) << '\n';
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

## Regular expressions

| Call | Result |
| --- | --- |
| `matches(pattern, text)` | Whether a match occurs anywhere; use anchors to constrain its position |
| `match_(pattern, text)` | Fixed TSL containing capture groups, excluding the whole match |
| `replace(pattern, replacement, text)` | Text with every nonoverlapping match replaced |

Patterns and replacement templates are fixed scalars and checked during construction.
Patterns use ICU syntax, including `(?<name>...)` for named groups. Replacement
templates use `\1`, `\g<name>`, or `\g<0>` for the complete match; `$` is literal.
These operations reject malformed UTF-8.

`match_` requires at least one capture group. Group 1 occupies index 0. An unmatched
group is invalid; a matched empty group is a valid empty string. Losing the match
invalidates every item. Capture guarantees limit const to final.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import matches

@graph
def valid_name(name: str) -> bool:
    return matches(r"^sensor-[0-9]+$", name)

print(run_graph(valid_name, ["sensor-12", "north"]))
```

Output:

```text
[True, False]
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

auto valid_name(wp<TS<string>> name) {
    return matches("^sensor-[0-9]+$", name);
}

int main() {
    const auto result = run_graph(valid_name, {"sensor-12", "north"});
    std::cout << std::boolalpha << result.equals({true, false}) << '\n';
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

## Split and join

`split(text, separator, maxsplit=-1)` returns a scalar list of strings. A negative
limit means unlimited splitting. The separator must be nonempty. Leading, trailing,
and adjacent separators produce empty pieces. Splitting and joining preserve bytes.

An explicit `TSL<TS<string>, N>` result requires `N > 0`. It consumes at most `N - 1`
separators and retains the remaining text in the last item. A smaller `maxsplit`
still limits splitting; fewer than `N` pieces raises an evaluation error.

`join(values, separator)` joins a scalar list or fixed time series list in item
order. A TSL requires a valid root and valid immediate items. `join(opt(values),
separator)` skips invalid items and treats an invalid root as empty. Optional join
has an ordinary output and evaluates on input changes. Empty input produces an
empty string.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, run_graph
from bg.operators import join, split

@graph
def path(text: str) -> str:
    return join(split(text, ","), "/")

print(run_graph(path, ["north,wind", "south,solar"]))
```

Output:

```text
['north/wind', 'south/solar']
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

auto path(wp<TS<string>> text) {
    return join(split(text, ","), "/");
}

int main() {
    const auto result = run_graph(path, {"north,wind", "south,solar"});
    std::cout << std::boolalpha << result.equals({"north/wind", "south/solar"}) << '\n';
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
