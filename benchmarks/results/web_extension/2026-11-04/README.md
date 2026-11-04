# Web extension loopback measurements — 2026-11-04

The native client/server graphs completed 368,640 measured exchanges across
HTTP, HTTPS, WS, WSS, and two HTTP controls. No production code changed.
The largest observed issue is HTTP connection churn at 16 concurrent clients:
HTTPS throughput falls to about 280 requests/s while each sample uses thousands
of distinct client ports. The current libcurl runtime retains eight idle
connections (`CURLMOPT_MAXCONNECTS`). Connection retention is the first follow-up
to investigate; these measurements do not isolate its cost from other costs.

Engine revision: `be009f7b3fdab2dbf78ea6cbff60cd6159009012`.
The [harness](../../../web_extension_benchmark.cpp) and this report are committed
together. [Environment and commands](environment.txt) record source/executable
hashes, dependencies, build flags, and reproduction details.

## Results

Each scenario has five samples of 4,096 exchanges. Throughput is the median
sample throughput. RTT percentiles pool the 20,480 chronological measurements
per scenario and use nearest rank (`ceil(p * n) - 1`). The window is the maximum
number of outstanding requests or messages.

### 64-byte bodies

| Protocol | Window | Exchanges/s | RTT p50 µs | RTT p99 µs |
|---|---:|---:|---:|---:|
| HTTP | 1 | 2,675 | 358.3 | 891.5 |
| HTTPS | 1 | 2,304 | 419.4 | 712.0 |
| HTTP | 16 | 10,529 | 1,360.9 | 4,029.0 |
| HTTPS | 16 | 282 | 64,699.0 | 125,494.9 |
| WS | 1 | 4,968 | 192.9 | 442.3 |
| WSS | 1 | 3,839 | 227.2 | 566.6 |
| WS | 64 | 21,665 | 2,245.0 | 6,685.9 |
| WSS | 64 | 16,631 | 2,633.2 | 10,479.1 |

### 4 KiB bodies

| Protocol | Window | Exchanges/s | RTT p50 µs | RTT p99 µs |
|---|---:|---:|---:|---:|
| HTTP | 1 | 2,184 | 429.0 | 815.0 |
| HTTPS | 1 | 2,388 | 402.7 | 888.3 |
| HTTP | 16 | 5,842 | 2,585.8 | 5,738.0 |
| HTTPS | 16 | 284 | 61,061.1 | 86,361.6 |
| WS | 1 | 4,072 | 195.9 | 507.9 |
| WSS | 1 | 2,789 | 342.5 | 587.2 |
| WS | 64 | 15,862 | 2,805.6 | 5,392.6 |
| WSS | 64 | 17,937 | 2,619.8 | 4,775.3 |

Cases run sequentially under varying host load. Small protocol differences,
including the 4 KiB WSS/WS ordering, need a controlled comparison before drawing
conclusions about transport overhead.

### Connection reuse and batching

Single-client HTTP and HTTPS use one peer port in every sample. At window 16,
the distinct peer-port ranges per 4,096-request sample are:

| Protocol | Body | Distinct peer ports/sample |
|---|---:|---:|
| HTTP | 64 B | 752–1,329 |
| HTTPS | 64 B | 3,220–3,651 |
| HTTP | 4 KiB | 1,800–2,851 |
| HTTPS | 4 KiB | 3,189–3,374 |

Ports can be reused, so these are lower bounds on connection counts. They show
substantial connection churn. Initial connections are warmed before timing;
connections opened during measured samples remain part of the measurement.

The WS/WSS window-64 cases use one connection each. Their mean server ingress
batch sizes are 2.88/1.76 messages for 64-byte bodies and 1.77/1.50 for 4 KiB
bodies. Maximum observed batches are 63/63 and 62/58, respectively. A window of
64 does not imply that the graph receives 64 messages in every batch.

### Raw HTTP client controls

A synchronous Beast client runs on one additional thread against the same
BG HTTP server and graph handler, with 64-byte bodies and window 1.

| Client mode | Exchanges/s | RTT p50 µs | RTT p99 µs |
|---|---:|---:|---:|
| Persistent connection | 6,078 | 151.8 | 308.6 |
| Connection close per request | 3,135 | 268.1 | 491.8 |

The persistent control retains its warmed connection across samples. The close
control creates 4,096 connections per measured sample; port reuse reduces the
distinct-port count to 3,880–3,926. These controls have a different client and
graph path, so subtracting their RTT from the BG-client RTT would not isolate
the BG client runtime cost.

## Method

The Release build uses GCC 14.3, shared libraries, an AMD EPYC 7R13 KVM VM,
and Linux x86-64. The process and its threads share affinity to CPUs 0–3;
threads are not individually pinned. TLS uses the test certificate as an
explicit CA file and verifies the hostname. The loaded runtime uses libcurl
8.20.0 and OpenSSL 3.6.2; the build uses 8.21.0 and 3.6.4 headers, respectively.
The environment artifact records the actual runtime library paths.

Each scenario runs 128 warmup exchanges and drains every warmup reply before
measurement. Graph wiring, startup, initial connections, and teardown are
outside timing. HTTP uses 1 or 16 separate service clients, with one request
outstanding per client. WS/WSS uses one service client and one connection,
with a window of 1 or 64 messages. The server echoes each complete message.

The graph thread drives the BG-client scenarios through ordinary queue nodes
and reply observers. There is no separate producer thread. RTT starts after
generating the payload and before constructing/writing the request literal;
it ends at the reply observer before response validation. Throughput includes
driver work between replies, scalar construction, validation, graph services,
queues, transport threads, and socket I/O. The WS handler also checks server
ingress order and forwards messages through `emit`.

Every body contains a sequence number, client ID, and deterministic remaining
bytes. The harness checks exact size and content, counts, duplicate replies,
client routing, and WebSocket order. Payloads change on every exchange.
The shared client-auth graphs, Python, external networks, startup latency,
and maximum server capacity are outside this measurement.

## Validation and limits

The clean Release smoke and full sweep both exit 0. The full artifact has
90 sample records, each containing 4,096 positive RTT measurements. A targeted
ASan build of the harness and service wiring sources passes the complete smoke
matrix. LeakSanitizer was disabled because it fails under the sandbox's ptrace
restrictions; this was not a leak check.

Smoke validation corrected harness node-template and borrowed-literal
lifetimes, initializer-list construction, warmup boundaries, and connection
counting before collecting the Release results.

Global CPU pressure was below 1% before the sweep and rose during measurement;
mid-run `some` pressure reached about 13% over 10 seconds and 15% over one
minute. Treat these as contention-affected VM measurements. Pressure snapshots
and CPU counters are retained alongside the results.

The full sweep emitted four closed-response-queue messages at graph shutdown
after replies passed validation. The [stdout extract](release-full.stdout)
retains those lines copied from the live tool output; the
[stderr log](release-full.stderr) records scenario progress. No runtime fix was
included in this benchmark slice.

The [raw JSONL](release-full.jsonl) contains per-sample throughput, p50/p95/p99,
connection/batch counters, and every RTT. Reproduce the recorded Release
configuration, then run with loopback proxy bypass:

```sh
env NO_PROXY=localhost,127.0.0.1,::1 no_proxy=localhost,127.0.0.1,::1 \
  taskset -c 0-3 build/web-benchmark/extensions/web/hg_web_extension_benchmark \
  --messages 4096 --samples 5 --output release-full.jsonl
```

Use `--quick` for a small validation run. The exact dependency paths and timed
command, including the runtime library path, are in [environment.txt](environment.txt).
