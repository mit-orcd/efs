# Test hosts page and wave gate script (Task T6, Nov 10)

**Merge state (Nov 10):** merged into devel in wave 73.

**Status (Nov 10): PASS.** Source commit `696dbad58` (devel `862a78945` plus the
page, script and pointer line). The trial gate on fstor007 ended
`TOTAL 599 FAIL 0 NOTRUN 3 STATUS COMPLETE (617s) HOSTLIMIT 3 BUILD 13s`.
Remaining failures: none. Active run: none. Next action: the coordinator uses
`scripts/wave_gate.sh` for wave 73. Merge state: branch
`claude/gracious-lovelace-65be40`, unmerged.

Purpose: structure review task T6 ([briefs](structure-review-tasks-20261110.md)).
Host access and the gate procedure lived only in task briefs. No W/D item or
release gate; verdict is for the task.

## What changed

- [`docs/how-it-works/test-hosts.md`](../../how-it-works/test-hosts.md) (117 lines): access, hosts, never-touch list, work layout, no `.git` in a shipped tree, the wave gate and the merge rules around it.
- `scripts/wave_gate.sh <host> <tar-on-host> <name>` (run on the host) and `--clean <name>`: extract, libfuse prefix by host, detached `screen`, quiet-host wait (`wait.log`), results with `SHA256SUMS`; refuses (exit 2) if the artifacts directory exists; trees deleted with `~/bin/edelete --force --delete` when present.
- One pointer line in `docs/how-it-works/testing.md`.

## Environment and trial

fstor007, 96 CPUs, load 1.2 before the run, libfuse shim `/tmp/efs-taskT6-fuse`,
`EFS_TEST_JOBS=8`. Tar of `696dbad58` (`git archive`), sha256 `b9a7b2b0…20f1d1410`
(`b9a7b2b098499597ac0f9887b961e2731a79e5c76554509918224d20d3b1d39f`), equal on both ends.

```sh
git archive --format=tar -o /tmp/t6.tar HEAD
scp <proxy opts> /tmp/t6.tar erbmi1@fstor007:t6.tar          # scp takes user@host, not -l
ssh ... fstor007 'tar -xOf ~/t6.tar scripts/wave_gate.sh > ~/wave_gate.sh; bash ~/wave_gate.sh fstor007 ~/t6.tar taskT6'
bash ~/wave_gate.sh --clean taskT6
```

Results:

- `wait.log`: `start: 10:04:57`, `waiting: load 1.45` (10:04:57), `waiting: load 1.32` (10:05:17), `end: 10:05:37`. Three quiet checks 20 s apart; the own tree `/tmp/efs-taskT6` matched the pattern and was ignored as required. Nobody else was running.
- `summary.txt` line 1: `TOTAL 599 FAIL 0 NOTRUN 3 STATUS COMPLETE (617s) HOSTLIMIT 3 BUILD 13s`. The three NOTRUN rows are host limits: `test_cluster_stop.py` (no `~/git/cluster`), `test_rdma_memcheck.py` (no `valgrind/memcheck.h`), `test_qualify_rdma_live.py` (no RDMA hardware).
- Second call with the same name and date: `... exists; rename it ... first`, rc 2, nothing moved.
- `--clean`: rc 0, `ls /tmp/efs-taskT6*` on fstor007: no such file. Tar removed on both ends.

Raw output stays outside Git: `fstor007:~/efs-artifacts/taskT6-20261110-fstor007/`
(`SHA256SUMS`, `summary.txt`, `timings.tsv`, `build.log`, `progress.txt`, `gate.log`, `wait.log`).

## Limitations

- Only fstor007 was exercised; the node9900 branch (prefix `/data1/efs/deps/libfuse`, the orphan `efs-taskDQ/m2/summary` exclusion) is untested.
- The wait only ever saw a quiet host; the busy path (counter reset) was not exercised.
- The trial ran the script from the tarball, not from a previously shipped checkout.
