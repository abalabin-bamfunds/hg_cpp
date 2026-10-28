# HTTP header conversion benchmark

The standalone benchmark compares the previous per-byte append conversion with
the current header helpers. It checks output equivalence for all inputs before
timing. Each reported value is the median of seven samples.

Run from the repository root with GCC 14.3.0:

```sh
c++ -std=c++23 -O3 -DNDEBUG benchmarks/http_header_bench.cpp -o /tmp/http_header_bench
/tmp/http_header_bench
```

Measured on 2026-10-29:

| Case | Previous | Current | Speedup |
| --- | ---: | ---: | ---: |
| Server, short ASCII | 53.7 ns/op | 29.7 ns/op | 1.81x |
| Server, long ASCII | 1089 ns/op | 189 ns/op | 5.75x |
| Server, short mixed Latin-1 | 79.8 ns/op | 40.8 ns/op | 1.96x |
| Server, dense Latin-1 | 1910 ns/op | 503 ns/op | 3.80x |
| Client, 24-byte ASCII | 54.0 ns/op | 16.9 ns/op | 3.19x |
| Client, 512-byte ASCII | 1028 ns/op | 294 ns/op | 3.50x |

Client ASCII timings reuse owned strings without copying between conversions.
