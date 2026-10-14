# Benchmark results — run 1791920655190335

Measurements follow Section 8 of `protocol_design_and_plan.md`. Latency percentiles are calculated from raw samples with nearest-rank.

## Exact wire size

| Message | Binary bytes | HTTP/JSON bytes |
|---|---:|---:|
| BID | 38 | 188 |
| CANCEL | 28 | 116 |
| ORDER_ACK_1_trade | 71 | 238 |
| ORDER_ACK_resting | 39 | 178 |
| UDP_market_data | 128 | — |

## Latency measurements

| Measurement | Codec | Scenario | Samples | p50 | Mean | p99 | p99.9 | Max | Unit |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| encode_decode_ns_per_message | binary | BID | 100 | 1419.902 | 1478.061 | 1939.846 | 1971.552 | 1971.552 | ns |
| encode_decode_ns_per_message | binary | ORDER_ACK_1_trade | 100 | 2389.297 | 2565.427 | 4234.976 | 4357.651 | 4357.651 | ns |
| encode_decode_ns_per_message | text_http_json | BID | 100 | 23897.580 | 24332.249 | 30043.142 | 30988.303 | 30988.303 | ns |
| encode_decode_ns_per_message | text_http_json | ORDER_ACK_1_trade | 100 | 49225.517 | 50068.477 | 56443.760 | 91932.602 | 91932.602 | ns |
| frame_bytes_to_queue_push_us | binary | in_process | 50000 | 0.094 | 0.102 | 0.135 | 0.223 | 103.847 | us |
| request_reply_us | binary | loopback | 3000 | 136.096 | 141.134 | 348.071 | 619.622 | 835.650 | us |

## Throughput

| Codec | Scenario | Operations per second |
|---|---|---:|
| binary | loopback_single_client | 3142.2 |

## Not measured

- Text protocol network RTT and throughput: no text-protocol dispatcher exists.
- Two-host LAN: run requires a second host.
- Idle-policy sweep: callback update delivery and connection churn are not measured.

No values are estimated for these items.

## Plots

- `round_trip_latency.svg` — binary loopback RTT.
- `dispatch_latency.svg` — complete encoded frame bytes through queue insertion.
- `binary_codec_latency.svg` and `text_codec_latency.svg` — in-process codec costs, not network RTT.
- `bid_codec_comparison.svg` — Binary vs Text HTTP/JSON BID codec latency overlaid on one graph.

`in_process` timings run codec/dispatch work locally without a network round trip. `loopback` timings send requests over TCP between the client and server on the same computer.
- PNG copies are also made when Matplotlib is installed.
