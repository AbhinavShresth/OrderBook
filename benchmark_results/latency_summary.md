# Benchmark summary — run 1791920655190335

Percentiles use nearest-rank over raw CSV samples. Units are shown per measurement.

| Section | Metric | Codec | Scenario | Samples | p50 | Mean | p99 | p99.9 | Max | Unit |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---|
| codec_cpu | encode_decode_ns_per_message | binary | BID | 100 | 1419.902 | 1478.061 | 1939.846 | 1971.552 | 1971.552 | ns |
| codec_cpu | encode_decode_ns_per_message | binary | ORDER_ACK_1_trade | 100 | 2389.297 | 2565.427 | 4234.976 | 4357.651 | 4357.651 | ns |
| codec_cpu | encode_decode_ns_per_message | text_http_json | BID | 100 | 23897.580 | 24332.249 | 30043.142 | 30988.303 | 30988.303 | ns |
| codec_cpu | encode_decode_ns_per_message | text_http_json | ORDER_ACK_1_trade | 100 | 49225.517 | 50068.477 | 56443.760 | 91932.602 | 91932.602 | ns |
| dispatch_latency | frame_bytes_to_queue_push_us | binary | in_process | 50000 | 0.094 | 0.102 | 0.135 | 0.223 | 103.847 | us |
| round_trip | request_reply_us | binary | loopback | 3000 | 136.096 | 141.134 | 348.071 | 619.622 | 835.650 | us |

Text network RTT is not present because this project has no text-protocol dispatcher. No text RTT values are inferred or synthesized.

Section 8 dispatch target (p99 ≤ 50 us): **PASS** (0.135 us).
