# Forward updates conditionally

To forward calibrated readings only while an enable input is true, use
`filter_(condition, value)`. Closing forwarding preserves the last output value.
The reading can continue changing while forwarding is closed.

## Forward while enabled

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import filter_


@graph
def reported_temperature(reading: int, enabled: bool) -> int:
    calibrated = reading + 2
    return filter_(enabled, calibrated)


print(test_graph(
    reported_temperature,
    [18, None, 19, 20, None, None, None, 20],
    [False, True, False, None, True, False, True, None],
))
```

Output:

```text
[None, 20, None, None, 22, None, None, 22]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto reported_temperature(wp<TS<i64>> reading, wp<TS<bool>> enabled) {
    const auto calibrated = reading + 2;
    return filter_(enabled, calibrated);
}

int main() {
    const auto result = test_graph(
        reported_temperature,
        {18, SKIP, 19, 20, SKIP, SKIP, SKIP, 20},
        {false, true, false, SKIP, true, false, true, SKIP});
    std::cout << std::boolalpha << result.equals({SKIP, 20, SKIP, SKIP, 22, SKIP, SKIP, 22}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [first program's CMake project](getting-started.md#write-and-run-the-example)
also builds this example. The file is `first_graph.cpp`, linked to `bg::library`.
:::
::::
:::::
::::::

| Step | Reading update | Enable update | Current calibrated value | Output update | Current output value |
| --- | --- | --- | --- | --- | --- |
| 1 | 18 | False | 20 | No update | No current value |
| 2 | No update | True | 20 | 20 | 20 |
| 3 | 19 | False | 21 | No update | 20 |
| 4 | 20 | No update | 22 | No update | 20 |
| 5 | No update | True | 22 | 22 | 22 |
| 6 | No update | False | 22 | No update | 22 |
| 7 | No update | True | 22 | No update | 22 |
| 8 | 20 | No update | 22 | 22 | 22 |

At step 2, the enable update opens forwarding. The result uses the current
calibrated value even though no reading arrives at that step.

The readings at steps 3 and 4 still reach the calibration calculation. When
forwarding reopens at step 5, it publishes the latest value, `22`. The intervening
value `21` is not replayed.

## Keep the last forwarded value

Closing forwarding at step 6 leaves the output at `22`. Before the first value
is forwarded, as at step 1, the result has no current value.

At step 7, reopening produces no update because the calibrated input has not
updated since its last forwarding. Repeated true condition updates also leave
an unchanged input unpublished.

At step 8, a new reading arrives while forwarding is open. Its calibrated value
is `22` again, and that update reaches the output. To suppress equal output
updates too, apply [deduplication](dedup.md) after filtering.

To read the current value on each separate request, use
[sampling](sample.md).

[Conditional forwarding reference](../reference/library-selection.md#filter_) specifies the full contract.
