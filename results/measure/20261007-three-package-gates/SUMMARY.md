# Three-package NUC acceptance evidence — Oct 7, 2026

See the [status ledger](../../../docs/status/three-package-gates-20261007.md)
for scope, source fixes and remaining gates. Private TCP fixtures only; no
normal-service rollout. Every source snapshot lives under `/data1/efs` on NUC.
Fixture JSON records the source/test/binary hashes and exact CLI options.

| Experiment | Outcome | Raw log |
| --- | --- | --- |
| Old KV production-cap idle recovery | FAIL: 120-second timeout | [before](three-gates-kv-before.log) |
| Repaired KV cap/idle recovery | PASS | [after](three-gates-kv-after.log) |
| Repaired KV four steady update cycles, NVMe | PASS | [steady](three-gates-kv-steady.log) |
| Final KV cap/drain/steady/reopen, including WAL repair, SATA | PASS | [final](three-gates-kv-final.log) |
| Old WAL concurrent-tail crash | FAIL: overwrite, insertion, deletion lost | [before](three-gates-wal-before.log) |
| Repaired WAL, complete KV unit suite | PASS | [after](three-gates-wal-after.log) |
| Production allocator full normal/drain reserve | PASS | [allocator](three-gates-final-bufpool.log) |
| Exact ancestry/participant production helpers | PASS | [namespace](three-gates-final-namespace-unit.log) |
| Production nested rename deadline helper | PASS | [deadline](three-gates-rename-unit.log) |
| Initial direct withheld publication + sparse pressure + GC/restart | PASS | [direct](three-gates-w59-direct.log) |
| Initial buffered withheld publication + sparse pressure + GC/restart | PASS | [buffered](three-gates-w59-buffered.log) |
| Final direct fault repeat, refused write does not extend file | PASS | [strict direct](three-gates-pressure-final-direct-repeat.log) |
| Final buffered fault repeat | PASS | [strict buffered](three-gates-pressure-final-buffered.log) |
| Final direct namespace boundary + restart + POSIX + GC | PASS: 216 single, 64 peer, zero failures, one skip | [direct](three-gates-final-direct.log) |
| Final buffered namespace boundary + restart + POSIX + GC | PASS: 216 single, 64 peer, zero failures, one skip | [buffered](three-gates-buffered-repeat2.log) |
| Discovery/retirement, including natural socket-close exit | PASS | [processes](three-gates-client-processes-repeat.log) |
| Full clean Linux `make -j8 test` | PASS | [units](three-gates-full-test-clean.log) |

## Commands

```
python3 tests/live/gc_reclamation.py --mode direct --namespace-max --namespace-boundary --namespace-cycles --posix --port 20810
python3 tests/live/gc_reclamation.py --mode buffered --namespace-max --namespace-boundary --namespace-cycles --posix --port 20850
python3 tests/live/gc_reclamation.py --mode direct --publication-pressure --client-binary ./efs-fuse --pressure --port 20870
python3 tests/live/gc_reclamation.py --mode buffered --publication-pressure --client-binary ./efs-fuse --pressure --port 20870
make -j8 test
cc -O2 -g -D_GNU_SOURCE -Iinclude tests/measure/kv_pressure_recovery.c libefs.a -pthread -lm -ldl -libverbs -o /data1/efs/three-gates-kv-final
/data1/efs/three-gates-kv-final /data1/efs/three-gates-kv-final-store /data1/efs/three-gates-kv-final-fault
```

Pressure client and engine experiments explicitly link `EFS_FAULTS=1`; normal
namespace fixtures use normal daemon/client builds. The production cap is not
lowered. The fault engine's temporary file parks only its own compactor.

## Retained failures and their classification

- `three-gates-w84-direct.log`: first test assumed a root-level source could
  fit all participants at depth 63. It cannot; later test discovers capacity
  with moves along the chain and records the actual envelope.
- `three-gates-w84-fixed-direct.log`: the live refusal exceeded the initial
  30-second bound. Shared eight-second rename deadline fixes nested retries.
- `three-gates-final-buffered.log`: process exits between stat and cmdline;
  discovery recheck repairs false malformed-client refusal.
- `three-gates-buffered-repeat.log`: natural retirement closes the control
  socket before joining the final worker; bounded natural-exit wait repairs it
  without weakening signal ownership checks.
- `three-gates-pressure-final-direct.log`: relative `./efs-fuse` Path loses
  the slash before Popen; explicit paths are now resolved before spawning.
- `three-gates-full-test.log`: the remote snapshot omitted results linked by
  the documentation. `three-gates-full-test-repeat.log`: copied macOS test
  executables could not run on Linux. Copying linked results and cleaning the
  private tree before rebuilding yields the successful clean full-unit run.

These failures remain available and are not passing acceptance evidence.
