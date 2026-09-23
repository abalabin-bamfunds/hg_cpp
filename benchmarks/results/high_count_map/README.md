# High-count map results

Each JSON file contains one validated run of the
[high-count map benchmark](../../high_count_map.md). The path identifies the
engine and the full source commit used to build its runtime:

```text
<engine>/<source-commit>[-<runtime>].json
```

Keep raw measurements. Add a file for a new source commit instead of replacing
an existing result. Use a runtime suffix when one source revision provides
several runtimes, such as `-cpp` and `-python`.

Every result must include `source_commit`, the benchmark profile, workload
shape, validation checksum, timings, latency classes, memory measurements, and
enough environment information to identify the operating system, processor,
compiler or interpreter, and CPU affinity. Record the benchmark adapter commit
when the implementation is maintained outside the engine source commit.

Schema version 1 adapters can report one combined `lifecycle` latency class.
New adapters report the four lifecycle phases defined by the benchmark.

Compare performance only between runs from equivalent environments. The source
commit key tracks changes within an engine; it does not remove machine noise.
