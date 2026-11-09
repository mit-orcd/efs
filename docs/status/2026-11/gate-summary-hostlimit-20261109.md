# Gate summary: host-limit NOTRUN rows and COMPLETE (Task ES)

**Verdict: PASS (Nov 9). `scripts/make_test_summary.sh` marks a NOTRUN row
whose reason is a known host limit, counts those rows `HOSTLIMIT n` on line 1,
and says `STATUS COMPLETE` (exit 0) when FAIL is 0, nothing ran over budget
and every NOTRUN is a host limit.** Replayed over three real gate summaries:
both wave 64 gates now read COMPLETE with HOSTLIMIT 3 and 4, the wave 65
node9900 gate-1 output still reads FAIL with the k54 row, and an added NOTRUN
with an unknown reason reads INCOMPLETE. No K row, blocks alpha: no. Not
merged, not pushed.

- Purpose: the wave ledger notes each wave that the gates' NOTRUN rows are
  standing host limits, and STATUS read INCOMPLETE on every gate because of
  them. A NOTRUN for any other reason must stay visible; this change keeps it
  so and stops the standing ones from hiding it.
- Source: base devel `33cd0a839` (wave 65). Branch
  `claude/elastic-yalow-a51daf`: code and test `62bbacd34`, this page and the
  ledger entry in the next commit, then the graph rebuild. Mac only, no build.

## The change

- `HOST_LIMITS` at the top of the script's Python block: one regex of the
  tests' own reason texts, `valgrind is not installed`,
  `valgrind/memcheck.h is not installed`, `valgrind missing`,
  `RDMA hardware runs NOTRUN here`, `is absent (EFS_CLUSTER_SCRIPTS)`
  (from `tests/test_rdma_memcheck.py`, `tests/test_leak_gate.py`,
  `tests/test_qualify_rdma_live.py`, `tests/test_cluster_stop.py`). Its
  comment says a new entry needs the ledger's standing note first.
- Line 1 keeps its field order: `TOTAL n FAIL f NOTRUN k STATUS s (Ns)`, then
  ` OVERBUDGET o` when nonzero, then the new ` HOSTLIMIT h` when nonzero,
  then ` BUILD Bs`. NOTRUN k still counts every NOTRUN row; h is the subset.
- STATUS: FAIL (exit 1) as before; PASS (exit 0) when there is no NOTRUN, as
  before, with or without OVERBUDGET; COMPLETE (exit 0) when every NOTRUN is a
  host limit and OVERBUDGET is 0; INCOMPLETE (exit 3) otherwise. A host-limit
  run with an OVERBUDGET row is therefore INCOMPLETE, as the brief asked.
- Row format: a host-limit row keeps `NOTRUN` in its first column and gets a
  fifth column `host limit`:
  `NOTRUN<TAB>176<TAB>command<TAB>mark<TAB>host limit`.
  **Deviation from the brief**, which spelled the row `NOTRUN (host limit)`:
  `scripts/refit_test_budgets.py:62` and
  `tests/measure/rdma_registry_close.py:135` select rows with
  `startswith('NOTRUN\t')`, so a changed first column would silently drop
  host-limit rows from their NOTRUN sets (the budget refit would fit a 0.3 s
  NOTRUN as a real run). Both files are outside this task's area and
  `refit_test_budgets.py` belongs to a parallel task. The trailing column
  keeps both readers and a `grep NOTRUN` working. `~/git/devops` and
  `~/git/cluster` have no reader of `summary.txt` or NOTRUN rows.
- `tests/test_make_test_summary_runner.py`: one known-limit case (COMPLETE,
  exit 0, `HOSTLIMIT 1`, the fifth column) and one mixed case (INCOMPLETE,
  exit 3).

## Replayed first lines

Inputs fetched read-only from fstor007 (`~erbmi1/efs-artifacts/`, NFS home
shared with node9900): `summary.txt`, `timings.tsv`, `progress.txt` of each
gate, hashes checked against the host's `sha256sum`:

| File | sha256 |
|---|---|
| `merge-wave64-20261108-fstor007/summary.txt` | `da386bf8e53a8a9898289cfa2ca80798311383b361948ad27f053d7138db8f33` |
| `merge-wave64-20261108-fstor007/timings.tsv` | `ea0ec3f8a5e4d400503508accb8cf7abde658ed4292aec1e73964423b715902a` |
| `merge-wave64-20261108-fstor007/progress.txt` | `46690b0e9fe5371dbf67c5b09bd07c4d33df5b0e0fd0e425c8126ef272c1e823` |
| `merge-wave64-20261108-node9900/summary.txt` | `bb427dba902782e9184c2086d0a72ab909bba11e75316d4e2ee289ca9aa5c44e` |
| `merge-wave64-20261108-node9900/timings.tsv` | `ba973486975f155eee403c2a6c5498b4a5e3bf6c18a149dd363923f67355819f` |
| `merge-wave64-20261108-node9900/progress.txt` | `53969297fa3e81e3bccd5ca197d65f0e0f9880fcb167208315be98f23c1ff6aa` |
| `merge-wave65-20261109-node9900-gate1/summary.txt` | `d047c44040eeb609ac63465d7461854c7c756b9772371f721ac1e201bd9a2f1e` |
| `merge-wave65-20261109-node9900-gate1/timings.tsv` | `363b7e689a22177e2417a3fde7284212d679e0750b66c85db53ff091cb85eff9` |
| `merge-wave65-20261109-node9900-gate1/progress.txt` | `14b8a3d617ccec004e64a86ea6252dde47050fbbfd96d6206c291454a93dbce9` |

Recorded on the host (line 1 of each `summary.txt`):

```
TOTAL 595 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (633s) BUILD 13s   wave 64 fstor007
TOTAL 595 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (624s) BUILD 13s   wave 64 node9900
TOTAL 596 FAIL 1 NOTRUN 4 STATUS FAIL (599s) BUILD 13s         wave 65 node9900 gate 1
```

Replayed, devel `33cd0a839` script (before) and `62bbacd34` (after); the
duration is the replay's own:

```
before  TOTAL 595 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (2s) BUILD 13s
after   TOTAL 595 FAIL 0 NOTRUN 3 STATUS COMPLETE (5s) HOSTLIMIT 3 BUILD 13s     exit 0
before  TOTAL 595 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (2s) BUILD 13s
after   TOTAL 595 FAIL 0 NOTRUN 4 STATUS COMPLETE (4s) HOSTLIMIT 4 BUILD 13s     exit 0
before  TOTAL 596 FAIL 1 NOTRUN 4 STATUS FAIL (2s) BUILD 13s
after   TOTAL 596 FAIL 1 NOTRUN 4 STATUS FAIL (5s) HOSTLIMIT 4 BUILD 13s         exit 1
synthetic: wave 64 fstor007 plus `NOTRUN: synthetic: no fuse3 on this host`
after   TOTAL 596 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (3s) HOSTLIMIT 3 BUILD 13s   exit 3
```

The gate-1 replay keeps `516  1  python3 tests/test_k54_member_down_fence.py`
as its failure row (the replay's command column is the stand-in command, the
recorded last line and `tmp/fail-516.log` are unchanged). Every replayed
NOTRUN row (176, 177, 181, and 575 or 576) carries `host limit`; the
synthetic row (595) does not.

## Reproduction

The summary step runs the commands itself, so the replay runs the script's
embedded runner (extracted exactly as `tests/test_make_test_summary_runner.py`
does) over a stand-in `cmds.txt`: each recorded NOTRUN row's index prints its
recorded mark, each failure row's index prints its last line and exits with
its status, every other index runs `true`. Serial, no budgets file, so no
OVERBUDGET rows are possible (the real wave 64 and 65 summaries had none; the
OVERBUDGET-with-host-limits branch is covered by reading, not by replay).
Save as `replay.py` outside the tree (sha256
`c5f524644318593331d6ccdb645a5076145729b720bcbd6fb6dc4d5f703d33dc`):

```python
# replay.py <make_test_summary.sh> <summary.txt> [extra-mark]
import os, shlex, subprocess, sys, tempfile
src, summ = sys.argv[1], sys.argv[2]
runner = open(src).read().split('python3 - <<"EOP"\n',1)[1].rsplit('\nEOP',1)[0]
rows = open(summ).read().splitlines()
total = int(rows[0].split()[1]); cmds = ['true']*total
for r in rows[1:]:
    f = r.split('\t')
    if f[0] == 'NOTRUN': cmds[int(f[1])] = 'printf "%%s\\n" %s' % shlex.quote(f[3])
    elif f[0].isdigit(): cmds[int(f[0])] = 'echo %s; exit %s' % (shlex.quote(f[3]), f[1])
if len(sys.argv) > 3: cmds.append('printf "%%s\\n" %s' % shlex.quote(sys.argv[3]))
d = tempfile.mkdtemp(prefix='replay-')
open(os.path.join(d,'cmds.txt'),'w').write('\n'.join(cmds)+'\n')
env = dict(os.environ, EFS_TEST_JOBS='1', EFS_BUILD_SECONDS=rows[0].rsplit('BUILD ',1)[1].rstrip('s'), TMPDIR=os.path.join(d,'tmp'))
rc = subprocess.run([sys.executable,'-c',runner], cwd=d, env=env).returncode
out = open(os.path.join(d,'summary.txt')).read()
print('exit', rc); print(out, end='')
```

```bash
for g in merge-wave64-20261108-fstor007 merge-wave64-20261108-node9900 merge-wave65-20261109-node9900-gate1; do
  python3 -I replay.py scripts/make_test_summary.sh gates/$g/summary.txt | sed -n 1,2p; done
python3 -I replay.py scripts/make_test_summary.sh gates/merge-wave64-20261108-fstor007/summary.txt 'NOTRUN: synthetic: no fuse3 on this host' | sed -n 1,2p
git show 33cd0a839:scripts/make_test_summary.sh > before.sh   # then the same loop on before.sh
```

Checks: `bash -n scripts/make_test_summary.sh`,
`python3 tests/test_make_test_summary_runner.py`,
`python3 tests/test_make_test_recipe.py` and `make docs-check` pass on the Mac.
Fetched copies lived in the session scratch area and were deleted with it;
the originals stay on the host at the paths above.

## Limits

- Not run in a live gate; the first real gate after the merge shows the new
  line on both hosts.
- A host limit whose reason text changes in its test drops back to a plain
  NOTRUN (INCOMPLETE), which fails safe.
