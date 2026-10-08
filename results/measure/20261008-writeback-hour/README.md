# Writeback-hour evidence

[Scope and results](../../../docs/status/writeback-hour-20261008.md).
Baseline `4198a9af` plus the focused active PUT poll/backoff changes. `put-live.log` contains the fixture source/binary manifest path and full outcomes; installed service restoration is recorded in `put-driver.log`.


Additional evidence:

- `final-budget-unit.log`: full AMD units with peer-health and standalone PUT fixes.
- `partial-reply-red.log`, `partial-reply-green.log`, `partial-unit.log`: actual production TCP partial-ACK failure and repair, plus full units.
- `partial-broad-live.log`, `partial-broad-driver.log`: POSIX/peer/append/sync/GC/restart acceptance and service restoration after the partial-reply fix.
- `put-default.log`: actual no-parent 30-second production PUT timeout and ownership checks.
- `pressure/`: default body/drain budgets, withheld publication, exact cold sparse bytes and recovery.
- `replay-bytes-r3/`: same-group old-object physical deletion and exact cold bytes.
- `replay-cross-r4/`: cross-group explicit bootstrap, conflicting digest refusal, old-object re-PUT, cold retirement query/resubmit and exact bytes.

Probe commands on a built Linux tree (TCP loopback only; no installed cluster touched):

```sh
cc -Wall -Wextra -Werror -Iinclude -Isrc/client tests/live/put_deadline.c libefs.a -lpthread -lm -ldl -libverbs -o /tmp/efs-put-deadline
/tmp/efs-put-deadline
/tmp/efs-put-deadline --default-budget
python3 tests/live/get_pair_deadline.py
```

- `get-pair/`: actual 60.2-second red / 5.0-second green GET pair, inherited 75 ms gate, full units, read integrity and mixed-read broad run.
- `memory/`: default-budget publication/drain and concurrent sparse pressure under 1 GiB/no-swap, manifests, exact cold recovery and zero OOM-event snapshots.
- `failed-cross/`: retained failed driver logs; private fixture stores/logs remain on AMD.
- `sanitizer-frame.log`, `sanitizer-get.log`: address/undefined-behavior sanitizer acceptance on the Mac.

- `final/`: latest-source full unit and AMD direct/TCP broad acceptance, exact manifest, integrated hung-DNS normal/sanitized gate, committed-document check and restored-service process inventory.
