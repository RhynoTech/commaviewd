# UI Export Worker Socket Timeout Benchmark

Date: 2026-09-22

## Decision

Use a 10 ms socket timeout for the isolated thread exporter. The timeout is a CommaView failure bound, not an openpilot requirement.

The complete subprocess experiment is preserved on branch `experiment/subprocess-exporter`. Master keeps only the isolated worker thread, bundled `orjson`, and stdlib fallback.

## Method

`tools/bench/commaview-export-timeout-bench.py` loaded the production Sunnypilot exporter template and tested 5, 10, 20, 50, and 100 ms limits in two conditions:

1. a healthy AF_UNIX receiver draining 200 representative 13 KB payloads;
2. a deliberately stalled receiver with a constrained send buffer and a 1 MB payload, repeated ten times to force and measure the timeout path.

The benchmark ran on the gateway host and on the comma4 ARM device. UI-facing `_offer_payload()` remained non-blocking because all socket work stayed on the isolated worker.

## comma4 results

| Timeout | Healthy frames | Healthy socket errors | Stalled recovery p50 | Stalled recovery p95 |
|---:|---:|---:|---:|---:|
| 5 ms | 200 | 0 | 7.031 ms | 15.471 ms |
| 10 ms | 200 | 0 | 11.986 ms | 12.392 ms |
| 20 ms | 200 | 0 | 22.111 ms | 22.970 ms |
| 50 ms | 200 | 0 | 52.082 ms | 52.254 ms |
| 100 ms | 200 | 0 | 102.061 ms | 102.255 ms |

All ten stalled trials at every value detected the blocked socket and returned the worker to idle. Healthy throughput showed no timeout-dependent failures. Five milliseconds was faster on median but had materially worse jitter on ARM. Ten milliseconds was the shortest consistent value and cuts worst-case worker blockage by about 40 ms versus the previous 50 ms setting.

Device evidence: `/data/commaview-bench/results/timeout-matrix-thread-final.json`.
