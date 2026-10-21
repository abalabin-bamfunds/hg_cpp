# Count and remember updates

To count received readings, use `count(value)`. To obtain the value from the
preceding reading, use `lag(value)`. Both advance on input updates; skipped steps
leave their history unchanged.

## Count readings

`count` publishes `1` for the first reading and increments for each later reading.
A repeated value still counts as another update:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import count


@graph
def reading_count(reading: int) -> int:
    return count(reading)


print(test_graph(reading_count, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, 1, 2, None, 3, 4]
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

auto reading_count(wp<TS<i64>> reading) {
    return count(reading);
}

int main() {
    const auto result = test_graph(reading_count, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << std::boolalpha << result.equals({SKIP, 1, 2, SKIP, 3, 4}) << '\n';
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

The result has no current count before the first reading. Placing
[deduplication](dedup.md) before `count` makes it count forwarded changes instead.

## Read the preceding temperature

`lag` publishes the previous calibrated reading when the next one arrives:

::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import graph, test_graph
from bg.operators import lag


@graph
def previous_temperature(reading: int) -> int:
    calibrated = reading + 2
    return lag(calibrated)


print(test_graph(previous_temperature, [None, 18, 18, None, 24, 22]))
```

Output:

```text
[None, None, 20, None, 20, 26]
```
:::::

:::::{tab-item} C++
:sync: cpp

```cpp
#include <bg/library.hpp>
#include <bg/testing/test_graph.hpp>

#include <iostream>

using namespace bg;
using namespace bg::testing;
using namespace bg::operators;
using namespace literal;

auto previous_temperature(wp<TS<i64>> reading) {
    const auto calibrated = reading + 2;
    return lag(calibrated);
}

int main() {
    const auto result = test_graph(previous_temperature, {SKIP, 18, 18, SKIP, 24, 22});
    std::cout << std::boolalpha << result.equals({SKIP, SKIP, 20, SKIP, 20, 26}) << '\n';
}
```

Output:

```text
true
```
:::::
::::::

| Step | Reading update | Calibrated update | Count update | Previous-temperature update |
| --- | --- | --- | --- | --- |
| 1 | No update | No update | No update | No update |
| 2 | 18 | 20 | 1 | No update |
| 3 | 18 | 20 | 2 | 20 |
| 4 | No update | No update | No update | No update |
| 5 | 24 | 26 | 3 | 20 |
| 6 | 22 | 24 | 4 | 26 |

The first calibrated reading at step 2 is retained for the next input update.
Step 3 supplies another reading, so `lag` publishes the preceding `20` and
retains the new `20`.

Step 4 produces no lagged update. At step 5, the preceding reading is still the
`20` received at step 3. Lag is measured in input updates; it does not advance
through skipped positions in a test sequence.

The last calibrated reading, `24`, remains retained at the end of the test. It
would be published when another reading arrives.

[Running calculations](running-calculations.md) use the same reading sequence to
maintain a total and a mean across the updates received so far.

[Count and lag reference](../reference/library-stateful.md) specifies the full contract.
