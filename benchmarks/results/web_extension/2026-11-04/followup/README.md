# Connection cache and one-connection WebSocket throughput

Increasing the HTTP client's connection cache from 8 to 64 removes the observed
HTTPS throughput collapse at 16 concurrent clients. The one-line experiment
sets `CURLMOPT_MAXCONNECTS` to retain more idle connections. The runtime source
and build were restored to cache 8 after the experiment.

## Connection cache

Both variants use the same engine revision,
`a2f3d723d695eeca5e4399fb7cc850349c5dd0ef`, with 16 clients and one outstanding
request per client. Each case has five samples of 4,096 requests after 128
warmup requests. Throughput below is the median sample throughput; p99 pools
all 20,480 RTTs using nearest rank.

| HTTPS body | Cache 8 requests/s | Cache 64 requests/s | Cache 8 p99 ms | Cache 64 p99 ms |
|---|---:|---:|---:|---:|
| 64 B | 326 | 9,492 | 79.42 | 4.51 |
| 4 KiB | 293 | 5,886 | 85.10 | 6.16 |

The gains are about 29× and 20×. Cache 8 uses 2,956–3,429 distinct peer ports
per 64-byte sample and 3,180–3,365 per 4 KiB sample. Cache 64 uses exactly
16 in every HTTP and HTTPS sample. Distinct ports are a lower bound on
connection counts because ports can be reused.

The larger cache removes connection churn for this concurrency. It does not
establish a suitable fixed cache size for larger client counts.

The first comparison also measured lower plain-HTTP throughput with cache 64.
A bounded HTTP-only check used five samples of 8,192 requests per case:

| HTTP body | Cache 8 requests/s | Cache 64 requests/s |
|---|---:|---:|
| 64 B | 15,021 | 11,566 |
| 4 KiB | 10,027 | 10,037 |

The 64-byte result remains unresolved. The cache-64 check ran under greater
host contention: its pinned cores were mostly 80–100% busy, with global load
average about 27.5 and CPU `some` pressure around 4–6%. Before the cache-8 check,
load average had fallen to about 19.4 and `some` pressure to 0.74%. These runs
do not isolate the cache's effect on plain-HTTP throughput. The 4 KiB rates are
approximately equal.

## One-connection WS/WSS throughput

Each case wires one WebSocket client and sends one connection request. The
window counts messages outstanding on that connection. Both endpoints and the
echo handler run through BG; this measures the complete adaptor/graph path.
The rates include payload generation and validation costs described in the
initial report. They do not establish raw socket capacity.

The table gives median sample rates and one-direction application payload
throughput. Every case has five samples.

| Body | Window | WS messages/s | WS MiB/s | WSS messages/s | WSS MiB/s |
|---|---:|---:|---:|---:|---:|
| 64 B | 1 | 8,596 | 0.52 | 7,844 | 0.48 |
| 64 B | 64 | 33,989 | 2.07 | 29,429 | 1.80 |
| 64 B | 256 | 42,666 | 2.60 | 28,092 | 1.71 |
| 64 B | 1,024 | 35,699 | 2.18 | 29,366 | 1.79 |
| 4 KiB | 1 | 6,881 | 26.88 | 4,744 | 18.53 |
| 4 KiB | 64 | 25,062 | 97.90 | 21,573 | 84.27 |
| 4 KiB | 256 | 26,268 | 102.61 | 18,825 | 73.53 |
| 4 KiB | 1,024 | 27,548 | 107.61 | 20,817 | 81.32 |
| 64 KiB | 1 | 8.4 | 0.53 | 8.3 | 0.52 |
| 64 KiB | 64 | 2,742 | 171.35 | 2,780 | 173.75 |
| 64 KiB | 256 | 3,042 | 190.15 | 2,322 | 145.13 |

Larger windows do not consistently increase throughput. For 4 KiB messages,
window 64 already gives about 98 MiB/s WS and 84 MiB/s WSS. Increasing the
window to 1,024 raises pooled median RTT from 1.70 to 37.38 ms for WS and from
2.01 to 45.44 ms for WSS. Individual sample rates vary substantially; the raw
data retain the ranges and complete RTT arrays. These measurements do not
identify a universally optimal window.

### Large-message latency finding

With 64 KiB messages and window 1, pooled median RTT is 117.81 ms WS and
118.53 ms WSS; p99 is 145.95 and 149.73 ms. The process takes 107 seconds,
including warmup and lifecycle, but consumes only 2.1 seconds of CPU time.
Host CPU pressure is low during these runs. This is a wait rather than CPU
saturation.

Neither WebSocket runtime explicitly sets `TCP_NODELAY`. Nagle/delayed-ACK interaction
is a candidate cause; no socket-option experiment was performed. Queued
64-byte messages also show long tails: window-64 p99 is 58.00 ms WS and
54.61 ms WSS, compared with below 0.3 ms at window 1. A targeted socket-option
comparison is the next diagnostic step.

The original 4,096-message and subsequent 256-message 64 KiB/window-1 attempts
were interrupted before a case completed because the measured rate implied
long runs. They contribute no samples to the table. The bounded run uses
64 messages per sample. For 64 B and 4 KiB bodies, windows 1/64 use 4,096
messages per sample, window 256 uses 8,192, and window 1,024 uses 32,768. For
64 KiB bodies, windows 64/256 use 4,096/8,192. Larger-window cases cover at
least 32 windows per sample to limit fill/drain effects. Warmup remains 128
messages, fully drained before measurement.

All 22 WebSocket cases complete with five valid samples each, totaling
1,106,560 measured exchanges. The HTTP cache comparisons and checks add
327,680, for 1,434,240 accepted exchanges in this follow-up. Interrupted setup
and probe runs are excluded. The WS/WSS sweeps have low observed global CPU
pressure; the HTTP comparisons have the load limitations described above.

## Method and artifacts

The [initial benchmark report](../README.md) defines the graph topology,
timing boundaries, payload validation, TLS setup, and runtime dependencies.
These follow-up runs keep that fixture and CPU affinity to CPUs 0–3.
They bypass environment proxies for loopback traffic. Each payload has a
sequence number and client ID; the harness checks size, content, routing,
duplicates, and WebSocket order.

The harness now accepts repeated `--protocol`, `--window`, and `--payload`
selectors. Omitting them keeps the original case matrix. Selected runs omit
the raw Beast controls. `payload_mib_per_second` counts application payload
bytes in one direction of the echo exchange, not summed bidirectional traffic
or protocol overhead.

The [environment record](environment.txt) contains commands, source and shared
library hashes, cache variants, CPU observations, and exit statuses. Changing
the cache changes `libhg_web.so`; the executable can remain identical.

Raw results:

- [Cache 8, HTTP/HTTPS](cache8-http-https-window16.jsonl)
- [Cache 64, HTTP/HTTPS](cache64-http-https-window16.jsonl)
- [Cache 8, HTTP-only check](cache8-http8192.jsonl)
- [Cache 64, HTTP-only check](cache64-http8192.jsonl)
- [WS/WSS 64 B and 4 KiB, windows 1/64](ws-64-4k-window1-64.jsonl)
- [WS/WSS 64 B and 4 KiB, window 256](ws-64-4k-window256.jsonl)
- [WS/WSS 64 B and 4 KiB, window 1,024](ws-64-4k-window1024.jsonl)
- [WS/WSS 64 KiB, window 1](ws-64k-window1-64x5.jsonl)
- [WS/WSS 64 KiB, window 64](ws-64k-window64-4096x5.jsonl)
- [WS/WSS 64 KiB, window 256](ws-64k-window256-8192x5.jsonl)

[Run environment snapshots](run-environment.txt) retain CPU pressure, load,
time, and per-core samples grouped by their original capture names.
