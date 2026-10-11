# ORCD packed metadata development gate · Oct 11, 2026

Private source overlay on fcstor006; installed daemons and mounts are unchanged.
The overlay starts with runtime source `c52a464391a685a766ee49aa76a162de58162a81`
and the source changes listed in `source-manifest.json`.

The tests exercise canonical authoritative packed references, mixed ordinary
delta trailers, real chunk-key identity binding, checksummed shared-object
placement, mapping/ledger/receipt atomicity, prepared ledger exclusion, failed
batches, neighbor retention, real publication promotion and durable replay,
abrupt WAL exit and cold compaction. The complete Linux unit gate is recorded in
`linux-unit-gate.txt` (exit 0, zero compiler warnings). These gates do not
activate mounted packed writes.

No distributed packed PUT/GET, client coalescing, physical shared-object deletion,
container support or files/second improvement is claimed here.
