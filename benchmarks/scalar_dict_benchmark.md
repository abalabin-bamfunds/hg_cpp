# Scalar dictionary benchmark

Build and run the native dictionary lookup and iteration benchmark:

```sh
cmake --preset benchmark
cmake --build --preset benchmark --target bg_scalar_dict_benchmark
build/benchmark/benchmarks/bg_scalar_dict_benchmark
```

The benchmark uses 32, 1,000, and 100,000 entries. Lookup results are the
median nanoseconds per hit for `find()` and `contains()` followed by `at()`.
Iteration results are median nanoseconds per full pass for `Scalar::for_each`,
building and consuming a vector snapshot, and consuming cursor batches of 16
and 64 entries. The final columns compare snapshot storage with each fixed
batch buffer. The cursor itself has one runtime-specific allocation per active
traversal.
