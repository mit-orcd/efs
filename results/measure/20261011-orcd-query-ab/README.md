# ORCD query-refresh pause diagnostic, October 11 UTC

Runtime remained `52bc8edb`; the existing 16-worker benchmark on fcstor007
(PID 1381139, screen `1334126.tiny-files`) continued without restart.
A bounded diagnostic changed commands policy from version 0 / mask 3 to
version 1 / mask 0, then restored mask 3 at version 2 in a finally block.
This temporarily paused derived `.efs` queries; it did not change file-data
durability, transport mode or metadata routing. No competing policy change
was observed. The final read verified the restored policy.

| Phase | Approximate window | Files/s |
| --- | ---: | ---: |
| Enabled before | 30 seconds | 409.51 |
| Disabled, first | 30 seconds | 438.79 |
| Disabled, second | 30 seconds | 442.86 |
| Enabled after | 30 seconds | 405.84 |

Fifteen-second settling intervals preceded the first disabled and final enabled
window. Counts came from the last progress line in screen hardcopies; the
script verifies the same benchmark process and monotonic count. Its progress
report batches introduce some boundary quantization. Mean disabled rate was
440.83 versus 407.67 enabled, about 8.13% higher. This single bounded sequence
suggests query refresh contributes contention. It is not a repeated matched
performance acceptance test, latency attribution, or evidence that queries
explain the remaining overhead. Live CREATE/apply waits and legacy fragment
padding still need their own investigation and repair.

`result.json` contains timestamps, exact counts, elapsed monotonic intervals,
policy replies and restoration evidence; `capture.txt` contains full diagnostic
stdout. Remote hardcopies remain at `/tmp/efs-w109-query-ab-mulcpajb/`.
