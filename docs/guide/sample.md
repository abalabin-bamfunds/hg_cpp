# Read a value when signalled

To read the latest calibrated temperature on a separate request, use
`sample(signal, value)`. A signal update publishes the current value. Reading
updates alone leave the sampled output unchanged.

## Sample the current reading

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import sample


@graph
def sampled_temperature(reading: int, signal: bool) -> int:
    calibrated = reading + 2
    return sample(signal, calibrated)


print(test_graph(
    sampled_temperature,
    [18, 19, None, 20, 21, None],
    [None, True, True, None, True, False],
))
```

Output:

```text
[None, 21, 21, None, 23, 23]
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

auto sampled_temperature(wp<TS<i64>> reading, wp<TS<bool>> signal) {
    const auto calibrated = reading + 2;
    return sample(signal, calibrated);
}

int main() {
    const auto result = test_graph(
        sampled_temperature,
        {18, 19, SKIP, 20, 21, SKIP},
        {SKIP, true, true, SKIP, true, false});
    std::cout << std::boolalpha << result.equals({SKIP, 21, 21, SKIP, 23, 23}) << '\n';
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

| Step | Reading update | Signal update | Current calibrated value | Output update |
| --- | --- | --- | --- | --- |
| 1 | 18 | No update | 20 | No update |
| 2 | 19 | True | 21 | 21 |
| 3 | No update | True | 21 | 21 |
| 4 | 20 | No update | 22 | No update |
| 5 | 21 | True | 23 | 23 |
| 6 | No update | False | 23 | 23 |

At step 3, another signal publishes `21` again. Each signal update is a request,
even when its value and the sampled value are unchanged.

At step 4, the calibrated input becomes `22`, but the sampled output stays at
`21`. When the reading and signal update together at step 5, sampling uses the
new calibrated value, `23`.

## An update is the signal

The signal's value does not decide whether sampling occurs. The false update at
step 6 still publishes the current temperature. This example uses a Boolean
signal, but `sample` also accepts other time series types for its signal input.

For a Boolean value that enables or disables forwarding, use
[conditional forwarding](filter.md).

## Wait for an available reading

The calibrated input must have a current value when a signal arrives. A signal
before the first reading
produces no output; a later reading alone still does not publish a sample.
Another signal can sample it once it is available.

Skipped signals preserve the last sampled value, as at step 4.

[Sampling reference](../reference/library-selection.md#sample) specifies the full contract.
