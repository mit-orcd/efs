# W166 — flush-deadline decision memo (October 26, 2026)

Report: branch `agent/devel-task-g`, code head `c71677190`, base `394f2b791`.
Gate `TOTAL`: NOTRUN on branch/base (Task G's pre-decision hold).
Evidence: `docs/status/w166-close-eio-flush-deadline-20261026.md`.
W166 REPRODUCED on unchanged base product binaries: 36 untraced close-EIOs,
12 forensic close-EIOs; no product fix implemented. Multi-host/RDMA NOTRUN.
**Decision required; W166 remains OPEN.**

## Decision memo — choose the owner of the wait

**Recommendation: B, bounded authority redirection with the existing
30-second outer flush deadline.** Approve that direction before implementation.
The observation changes the proposed scope: faster *leader* fencing alone
cannot fix a lagging storage member after leadership already moved.

The untraced baseline produced 36 `close(2)` EIOs at 29,999.56–30,001.29 ms.
Their PUT stage consumed the budget; REPORT took 0–1 ms. At the first failure,
node 1 was already a follower with 1,497 unapplied entries; node 3 led group 0
and the other two replicas were fully applied. At that first failure the
unapplied count was below the 3/4 watermark: 1,497 versus 12,288 of node 1's
16,384-entry cap. Its KV capacity gate had
already refused proposals. This is an availability failure under the injected
single-replica pressure, even with a working quorum.

The separate traced run also exposed four shorter failures: PUT completed
in 154–379 ms, then REPORT spent 8 seconds and receipt resolution another
8 seconds before returning EIO with an unknown outcome. Eight additional
closes exhausted the outer 30 seconds in PUT. Changing only the outer
deadline would leave the inner limits unchanged.

| Choice | User latency / retained data | Failure behavior / code required |
| --- | --- | --- |
| **A: progress-bounded deadline renewal** | Can exceed today's 30 seconds if approved; dirty bytes, tickets and publication identities stay owned for longer. Requires a hard total cap and bounded in-flight memory. | A heartbeat, BUSY response, PENDING receipt or another replica applying is insufficient evidence that this operation is advancing. A genuinely parked replica makes no progress, so renewal alone cannot solve this reproduction. Requires operation-relevant progress evidence, cancellation/ownership rules and coordinated PUT, REPORT and receipt budgets. W159's server-local 400 ms grants / 2 s own-entry cap are a precedent, not an existing client protocol. |
| **B: earlier refusal and authoritative redirection** | Keep the 30-second ceiling and leave time for publication. Avoid spending it repeatedly asking a replica that cannot serve the required authority. | First investigate forwarding the **complete existing PUT-ticket STATUS/session check** to a healthy authoritative voter when a hosted replica cannot answer. The server already forwards that request for non-hosted groups. Preserve fresh ReadIndex, exact ticket/session checks and the local storage fence. Separately route REPORT/receipt work around an incapable coordinator; the four shorter failures need their own deterministic gate. Prevent forwarding loops, stale hints and unsafe retry/receipt conclusions. |

Prefer B because the healthy quorum already has the authority needed to
continue; waiting longer for the parked replica adds latency without making
it catch up. This recommendation does **not** authorize treating BUSY as a
dead member, acknowledging an undeclared two-fragment stripe, weakening
session fences or changing full-member/ENOSPC policy. Keep the existing
degraded-publication and rebuild contract for an actual unavailable member.
If safe redirection cannot meet the bound, return that evidence for a further
decision rather than silently extending it.

**Decision requested:** authorize B with the 30-second bound unchanged, covering
both the lagging-replica PUT authority path and the separate REPORT/receipt
path? Task G §2 requires stopping here until that choice is made.

## Reproduction and exact ladder

Production source/build: **devel `394f2b791`**, with no C changes.
Diagnostic driver `4a0d7460f` for the untraced run; `7cdbe0f55` for the separate
forensic run. Trace summarizer `c71677190`. No policy option is implemented.

Environment: xorinox, 16 logical CPUs, Linux
`6.12.0-211.67.1.el10_2.x86_64`, XFS home filesystem, user `maik` (UID 1000),
system libfuse 3.16.2. Three fault-build loopback members, free ports, 1 GiB
quota/member, two writers/member, TCP, O_DIRECT; one FUSE mount seeded at
healthy node 2. Four 2 KiB payload writers and four 16-worker metadata-load
processes mirror K22. Only node 1 has the 2 MiB L0 / 1 MiB memtable fault
budgets and parked compaction; the derived KV budget is 4 MiB. Other nodes
use defaults. Existing RPC/REPORT/write profiling switches are trace-only;
the forensic run adds PUT-ticket logging and strace. No new setting.

Build identity:
`efsd 394f2b791 (branch devel, built 2026-10-27T01:15:22Z on xorinox)`.
`identity.txt` retains the fault-daemon, client, management and bench binary
hashes plus build/compiler identity. No existing cluster, `efs` user,
protected port, or `/data1/efs` tree was touched.

| Experiment | Result | Successful-close payload verification |
| --- | --- | --- |
| Base, no syscall tracing, 300 s pressure | **FAIL / REPRODUCED**, rc 1: 36 close EIOs; zero open/write errors; median failed close 30,000.51 ms. Total 312.05 s including drain/verification/cleanup. | All 331 confirmed files exact |
| Same product binaries, separate 120 s forensic capture | **FAIL / REPRODUCED**, rc 1: 12 close EIOs, four REPORT/resolve failures at 16.15–16.38 s and eight PUT failures near 30 s. Total 132.65 s. | All 228 confirmed files exact |

The traced timings are diagnostic, not a performance comparison or a clean
rerun. Failed files are not promoted to successful writes, and no rollback
or durability conclusion is drawn from later visibility. Reads above cover
all files whose closes succeeded, not every unconfirmed failed attempt.
Readback used the same mount after pressure ended; fresh-client, crash and
physical-durability qualification are NOTRUN.

One complete 30-second forensic ladder, inode **237763**, ticket sequence
**229**, object **0x759a0134f4ab0209**, chunk 0:

1. Fragments 0 and 1 each have one complete send, to healthy nodes 2 and 3.
   The client records two ACKs; that is not a COMMIT/durability proof.
2. Fragment 2 is sent **66** times to lagging node 1. The same ticket/object
   identity is retained. First/last send epochs are
   `1793064342.924233` / `1793064372.623914`; median gap **452.55 ms**.
3. Node 1 logs **66** corresponding `put-upload refused fi=2 seq=229 rc=-15`
   results. Its STATUS log names NOT_PRIMARY and a healthy-leader hint.
   `efs_put_reply_status` maps that verdict to PUT BUSY; the data reply does
   not carry the hint. K34 retries the missing fragment, retaining earlier
   ACKs. The approximately 400 ms authority wait plus 50 ms retry pause is
   consistent with the trace; all 66 send times remain in the analysis JSON.
4. `CLOSE-PROF`: total **30001 ms**, PUT **30001 ms**, REPORT **0 ms**, rc -13.
   `efs_fuse_flush_ino` exposes EIO. No REPORT receipt can prove publication
   for this not-yet-reported close.

The other class is explicit too: inode **122898** PUT **379 ms**, REPORT plus
resolution **16000 ms**, total **16379 ms**, rc -1. Its logged publication
`75a61127/1/18e23e801b2fa0f0:244` exhausted a 16-attempt BUSY/STALE ladder;
resolution recorded four queries, two resends, `none=1/2`, and **unknown**.
An absent receipt or unknown outcome is not an abort proof.

The client's `published degraded for rebuild` line occurs in the local
`degraded_record` bookkeeping before publication. It cannot establish that
metadata was published or that the failed close met durability requirements.

## Commands, retention and remaining gates

Create a fresh private source tree under `$HOME`, using `git archive`
(no macOS filesystem tar), then build and run in screen:

```sh
make -j8 EFS_GIT_ID=394f2b791 EFS_GIT_BRANCH=devel \
  EFS_GIT_DESCRIBE=394f2b791 all tests/faults/efsd
TMPDIR="$HOME/efs-taskG-20261026/tmp" python3 -u scripts/w166_flush_diagnostic.py \
  --work "$HOME/efs-taskG-20261026/run-untraced" --seconds 300
# Separate fresh fixture and diagnostic-only tracing, after the first exits:
TMPDIR="$HOME/efs-taskG-20261026/tmp" python3 -u scripts/w166_flush_diagnostic.py \
  --work "$HOME/efs-taskG-20261026/run-traced" --seconds 120 --trace-client
python3 scripts/w166_trace_summary.py \
  "$HOME/efs-taskG-20261026/run-traced" --ino 237763
```

Use the recorded driver revisions when reproducing these exact experiments;
a fresh experiment has different inode/ticket values, selected from its own
application log and CLOSE-PROF lines. Driver exits 1 on reproduced close
failure, 2 if not reproduced, 3 on harness failure; it never retries a failed
experiment into PASS. The writer records an `fstat` before each write for
inode correlation; these are diagnostic workloads, not POSIX gate timing claims.

**Retained on xorinox:** `/home/maik/efs-artifacts/taskG-20261026/` (61 files,
about 30.4 MB), including both complete log sets, application/status JSONL,
socket traces, exact driver copies, identity/build log and per-ticket analysis.
All copied closed files were hash-checked before removing bulk. `SHA256SUMS`
itself: **90aa33419088b2c500630233e494524ce1eda16274f08d3a00e68627afd76a43**;
every listed hash verified. `PRUNED.txt` records deletion of the private source,
build, stores, payloads and compiler scratch with `~/bin/edelete --force
--delete`, after checked child shutdown and kernel mount absence. No raw
capture is committed. No private workload remains running on xorinox.

**NOTRUN pending the Task G decision:** implementation, a base-failing
deterministic acceptance test, five standalone K22 passes, branch/base
`make test-all`, pooled/failover/one-member-down gates, multi-host and RDMA.
There is no clean gate TOTAL to report and no FIXED/DECIDED claim. The new
scripts are standalone diagnostic drivers, not additional `make test-long`
commands; no budget row or D44 target is changed.
