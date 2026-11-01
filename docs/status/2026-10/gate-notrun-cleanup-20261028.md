# Task W — make supported gate-host coverage run

**Task W coverage PASS**, branch `agent/devel-task-w`, tested `f1062b969`,
base devel `3debca7ec`; not merged. Intel2 final gate: **523 / 0 FAIL / 10
NOTRUN**, **INCOMPLETE** for nine RDMA-device checks and missing Memcheck
headers. Baseline: **522 / 2 FAIL / 12 NOTRUN**. Both requested missing cases
now execute. Failed evidence is preserved; private scratch is removed. No
packages were installed. Product behavior and existing clusters are unchanged.
This contributes to W139, not full RDMA/sanitizer or alpha qualification.

## Changes and scope

- Provision the cluster-stop/remount test's six required source scripts from
  cluster repository commit `301e21671afec9025e8a1f08735e49bf6cac2a0c`. The Mac
  repository is read-only; its unrelated local testing edits are not exported.
  Only committed named files are exported, with hashes/provenance; no deployment
  configuration or credentials are copied. Tests use mocked transports.
- Build a dedicated fault-linked follower-bound binary and run its real parked
  compactor subcase explicitly in `make test-long`. The normal binary no longer
  advertises missing fault coverage; its wrong-binary argument fails explicitly.
- Repair the newly unwired graph-validator entry: `make code-graph-check` is a
  named developer gate requiring a generated graph and Git index, not a pretend
  hardware skip in an archived runtime-gate tree.
- Document exact RPM prerequisites and distinguish synthetic refusal self-tests
  from actual unavailable detectors. Existing leak/sanitizer self-test corrections
  on devel are retained; genuine runtime failures stay visible.

## Read-only package inventory (October 28)

All four hosts report Rocky Linux 10.2 x86_64, GCC/runtime packages
`14.3.1-4.4.el10`, Valgrind `3.26.0-5.el10`, FUSE `3.16.2-5.el10`, and
`ptrace_scope=0`. GCC, gcc-c++, glibc-devel, libasan, libubsan and libtsan are
installed on all four. Package availability is not detector qualification.

| Host | Valgrind executable | `valgrind-devel` / Memcheck headers | ASan/UBSan/TSan runtime packages |
| --- | --- | --- | --- |
| intel1 | installed | **missing** | installed |
| intel2 | installed | **missing** | installed |
| nuc | installed | **missing** | installed |
| xorinox | installed | installed | installed |

Task W makes no package/kernel/ptrace change. The user can decide whether to
provision the three missing header packages. RDMA device paths remain separate
hardware prerequisites and will not be turned off or counted as passing.

## Untouched baseline (before provisioning)

```text
TOTAL 522 FAIL 2 NOTRUN 12 STATUS FAIL (685s) OVERBUDGET 7 BUILD 38s
```

The baseline was copied from exact devel `3debca7ec`, with no cluster-script
fixture at the default path. Its two failures remain failures:

- `tests/test_w130_dir_fault.py`: the serve-first subcase passed, then the
  regular subcase's `raft-resync` request returned BUSY (`rc=-13`) and the test
  stopped. This is not NOT_PRIMARY (`-15`); no listing-loss or payload-loss
  conclusion follows from this particular run. Task W does not fix or waive it.
- `tests/test_make_test_recipe.py`: newly added `tests/test_code_graph.py`
  was neither in `test-all` nor listed as an explicit separate gate. Task W
  repairs that bookkeeping and actually runs its developer gate, rather than
  merely suppressing a check.

The 12 NOTRUN commands were the cluster-script prerequisite, Memcheck headers,
nine unusable RDMA paths (declaration, xprt, seven segmented modes), and the
normal-build stalled-compactor arm. The historical brief's 14 is not today's
measured baseline. Earlier devel changes already separated the intentional
missing-tool refusal controls from actual unavailable detector coverage; Task W
keeps those self-tests intact. No package/runtime detector is disabled.

## Focused candidate checks

Exact source `77f6badc0`, private `~/efs-taskW/candidate` on intel2. Four lab
hosts are inventoried read-only; builds and product tests run only on intel2.
No user daemon, mount or `/data1/efs` store is involved. Product `src/` and
`include/` are unchanged from base.

| Check | Result | Wall seconds |
| --- | --- | ---: |
| Cluster stop/remount, actual committed scripts with mocked transports | PASS | 0.1484 |
| Normal follower-bound arms | PASS, no fault-arm NOTRUN marker | 0.0208 |
| Dedicated fault-linked stalled compactor | PASS | 1.0265 |
| Fault-only request to normal binary | expected refusal rc 2 | 0.0038 |
| Explicit missing cluster prerequisite | expected failure rc 1 | 0.0332 |
| Recipe/coverage/budget validation | PASS | 1.0049 |

The fault log proves real `kv-fault: compactor parked` and release, apply
backpressure and 9,865 deferrals. The healthy quorum commits 6,001 entries
(6,000 writes and its no-op); the follower catches up to 6,001 exactly. Peak
unapplied log 897 is below cap 1,024. Memtable peak 263,040 bytes and L0 peak
1,069,980 bytes meet the original test's unchanged one-batch allowances
(budget 262,144 bytes and L0 cap 1,048,576); no bound was loosened.
The first eight-job gate measured the new case at 1.2 s (final gate: 2.3 s). `f1062b969` replaces
its provisional 15 s / one-slot budget with **5 s**, 1.25× rounded up to five
seconds. Target remains 1,100 s; the recipe guard remains 1,650 s including its
margin. No existing test budget or deadline is raised.

## Separate developer graph gate

`make code-graph-check` rebuilt the canonical graph and passed **9/9** checks
on the Mac's existing pinned graphify environment at `77f6badc0`. It validates
navigation and deliberately rejects missing/reversed production edges and
missing/changed oracle evidence; it is not product runtime qualification.
A bare `git archive` may include the graph snapshot, but its tracked-scope
check lacks the Git index; rebuilding also needs the pinned tool. The explicitly named separate gate is recorded in
`NOT_WIRED`, like other developer/release-only checks, with a real target and
reason. No runtime NOTRUN is manufactured for it.

The reviewed graph candidate and original check output are retained outside Git
at `/Users/mike/Documents/Codex/artifacts/taskW-20261028/code-graph-check/`,
manifest SHA-256 `0328c80fdddc8a48cb2857403d946b5c99ec698cd8baffc3f698d4757327aa56`.
The final source (`f1062b969`) was rebuilt and checked again: 9/9 PASS,
with the candidate retained at
`/Users/mike/Documents/Codex/artifacts/taskW-20261028/code-graph-final/`,
manifest SHA-256 `2416dd8cd9cd196bc2934558fcf4c77353ab88f35a44405c3d0fc3d1dd4af669`.
The extractor reports partial parsing of `include/efs/protocol.h`, 49 files
without symbols and 1,353 unresolved calls: these are navigation coverage
limitations, not runtime proof. The branch does not publish a large regenerated
navigation snapshot; the merge workflow can rebuild it after integrating source
changes.

## First full candidate and test correction

```text
TOTAL 523 FAIL 1 NOTRUN 10 STATUS FAIL (754s) OVERBUDGET 4 BUILD 0s
```

At `77f6badc0`, cluster-stop ran in 0.1 s, the fault-only follower arm in 1.2 s,
and the recipe check in 1.0 s. Baseline W130's resync BUSY did not recur; that
single later success does not close its original failure. The four budget
observations were fsync-degraded 80.2/80 s, stop-progress 64.0/50 s,
coordinator-kill 54.2/25 s and leader-admission 42.3/30 s. They are retained,
not used to increase budgets.

`tests/test_dir_ops_busy.py` failed because it required exactly 21 transport
attempts. The single RMDIR succeeded in 13.56 s after exactly 20 injected
pre-admission refusals (remaining 19 through 0), all with the same operation
sequence, but reported 22 attempts. Ordinary retryable routing/admission
responses are not limited to the injected refusals. This is a **test defect**,
not an application-visible EBUSY or EIO. Source inspection of
`rpc_send_recv_dual_once` confirms its counter includes connection/send retries,
NOT_PRIMARY redirects and retryable STALE responses; the capture does not
identify which supplied the extra attempt. `f1062b969` requires at least 21
attempts for the successful RMDIR while retaining the exact 20 fault hits,
single immutable operation sequence, one syscall without application retries,
zero marker, final absence and unchanged 30 s deadline. It neither accepts an
application error nor retries a failed test into PASS. The failed first gate is
never relabelled; a new full gate tests the correction.

## Final full gate (`f1062b969`)

```text
TOTAL 523 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (683s) OVERBUDGET 4 BUILD 30s
```

No retry of a failing test is substituted into this result. It is a new complete
run of the committed test correction. The original two failed full runs retain
their FAIL verdicts. No product source or header differs from the base.

| Requested check | Final measured result |
| --- | --- |
| Actual cluster stop/remount scripts, mocked transports | PASS, 0.2 s |
| Fault-linked real stalled compactor | PASS, 2.3 s; five-second budget unchanged |
| Directory BUSY contract with corrected attempt assertion | PASS, 18.5 s total fixture runtime |
| Recipe/coverage/budget check | PASS, 0.9 s |

The final ten NOTRUNs are exactly Memcheck headers, the declaration RDMA frame
path, RDMA xprt and seven segmented RDMA modes (normal, interrupt,
TCP-violation, deadline, burst, burst-asym, churn). They are unavailable host
coverage, never PASS. The full runner exits **3**, as required for INCOMPLETE.
There is no extra Task W ORCD gate: this task changes host setup/test wiring;
real RDMA remains explicitly unqualified here.

Four existing runtime-budget observations remain visible: stop-progress
75.3/50 s, coordinator-kill 55.4/25 s, query-delta 11.0/10 s and
leader-admission 42.7/30 s. Their correctness assertions passed. No budget is
raised to hide them. Baseline W130 resync BUSY remains unresolved even though
both candidates passed that case; the original store/log evidence is retained.

Final binary identity:

```text
efsd f1062b969 (branch agent/devel-task-w, built 2026-10-28T19:30:22Z on intel2)
```

## Environment and reproduction

intel2 is an eight-core Intel Core Ultra 7 256V, about 15 GiB RAM, no swap,
Rocky 10.2 kernel `6.12.0-211.67.1.el10_2.x86_64`, with the private home run trees
on XFS. Compiler defaults are the Makefile's GCC `-O3 -g`, native tuning and
frame pointers, using system FUSE 3.16.2 (`/usr/include/fuse3`, `-lfuse3`).
Normal library/client and fault-enabled server/KV overrides are
built by `make test-bins`; there is no global product `EFS_FAULTS` override.
Tests own ephemeral loopback clusters/ports and fixtures; the directory BUSY
case uses four private TCP/O_DIRECT daemons, two writers and 1 GiB quota each.
Other tests retain their per-test settings. No persistent cluster is redeployed.

Export the committed external prerequisite **before the candidate gate**, but
not before the baseline. Only if the target default directory is absent:

```sh
# On Mac; read-only export, no dirty external changes included.
git -C /Users/mike/git/cluster archive \
  301e21671afec9025e8a1f08735e49bf6cac2a0c \
  cluster.sh ct-up.sh gw-up.sh \
  lib/client-stop.sh lib/client-user.sh lib/transfer.sh \
  | ssh intel2 'mkdir -p ~/git/cluster && tar -x -C ~/git/cluster'
```

Record SHA-256 for each of the six files and verify against that commit; Task W
also writes `TASK-W-PROVENANCE.json` and makes the six files read-only. This
minimal script export is deliberately retained at `/home/maik/git/cluster` for
future gates. It has no `env.sh`, deployment configuration, credentials or Git
metadata. If a real checkout already exists, use it read-only or place the
export elsewhere and set `EFS_CLUSTER_SCRIPTS`; do not overwrite it.

For each source below, create a **new** private archived-source directory and
run in a detached screen with `SCREENDIR=$HOME/.screen` (create mode 700 first):

```sh
# On Mac in the Task W Git worktree, choose exactly one revision/destination.
git archive f1062b969 | ssh intel2 \
  'mkdir -p ~/efs-taskW/final && tar -x -C ~/efs-taskW/final'
# On intel2, inside the detached screen; use revision-specific output paths.
cd ~/efs-taskW/final
unset EFS_CLUSTER_SCRIPTS
export EFS_GIT_ID=f1062b969 EFS_GIT_DESCRIBE=f1062b969
export EFS_GIT_BRANCH=agent/devel-task-w
export EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8
scripts/make_test_summary.sh > ../final-controller.log 2>&1
# Record the exit status immediately; retain summary/timings/cmds/progress,
# build/enum logs, tmp/fail-*.log and ./efsd --version outside the build tree.
```

Baseline substitutes `3debca7ec`, `devel` and `~/efs-taskW/base` (external
scripts absent); first candidate substitutes `77f6badc0` and
`~/efs-taskW/candidate` (scripts installed). Build identity is passed explicitly
because an archive has no Git metadata. No failure is overwritten. The focused
commands are `python3 tests/test_cluster_stop.py`, `./tests/test_follower_bound`,
`./tests/faults/test_follower_bound --stalled-compactor` and
`python3 tests/test_make_test_recipe.py`. Negative controls use the normal
binary with the fault argument (must return 2) and an explicit nonexistent
`EFS_CLUSTER_SCRIPTS` directory (must fail, not skip).

## Retention and cleanup

Raw on-host evidence: **intel2 `/home/maik/efs-artifacts/taskW-20261028/`**,
with separate `baseline-3debca7ec`, `focused-77f6badc0`,
`candidate-77f6badc0` and `final-f1062b969` directories, package inventory,
external-script provenance and exact archived-source manifests. Verified closed inner manifest SHA-256 values:

| Run | Manifest SHA-256 |
| --- | --- |
| baseline `3debca7ec` | `6692d75e8fbaa2ad2b21fa256211a6a2d865835649e2e0af3d744b4710999888` |
| focused `77f6badc0` | `517330ee20d0f571867443f02de875eddbf2ba6105db6040da671d2ca1ffa999` |
| first full candidate `77f6badc0` | `c58289f6d46cc727df68aaa2d0c7ace3424673c3ad54854f1d43a072f762ef39` |
| final `f1062b969` | `d6726b065b2354b444d4b12a323477d5072fe414cd0a78fdcbb96eced1f06d5f` |

The sealed root `SHA256SUMS` covers **785 files**, including all inner
manifests, read-only package inventory, build/binary identities and hashes,
run helpers, cleanup receipts and the open baseline fixture. Its SHA-256 is
`6b240ea1d10607fc17ea516f9eaf45f385500f9469cb3f85a1385226851bedb6`.
Every listed file was verified after pruning. Exact archived source manifests
were also checked before pruning: base 2,280 files, each candidate 2,281 files.

**Open investigation bulk exception:** the original baseline W130 resync-BUSY
fixture is preserved on intel2 at
`/home/maik/efs-artifacts/taskW-20261028/open-baseline-w130-resync-busy/`,
713 files, with manifest `open-baseline-w130-SHA256SUMS`, SHA-256
`498029dc6b9729a1dcba9946a4eb307e93e01782fefe8a4a9f91fd7baaa29b2a`.
Every byte was verified before/after its move, with no private processes or
mounts remaining. This is retained only for the unresolved BUSY investigation;
inspect a copy if replay needs to mutate stores. W130's pre-existing listing
loss investigation is not resolved by this test run.

The failed first candidate's fixture was also hashed and preserved before the
new gate (83 files, manifest SHA-256
`30a2c38b22d595233a890913d8c47554d77fe7308d9345960e13c0dc2a6b068d`).
Its test-only failure is documented and its bulk was pruned while its full
failure/daemon/client logs, manifest and preservation receipt remain. The
baseline exception above is kept. No failure is erased from the results.

**Cleanup completed:** all three source manifests were verified, no private
mount or process executable/cwd remained, and
`~/bin/edelete --force --delete /home/maik/efs-taskW` removed only that private
scratch root (sources/builds/synthetic success fixtures). The source manifests
and binary hashes allow exact rebuilds; bulk is intentionally not kept as a
release artifact. `PRUNED.txt`, `cleanup-before.json`, `cleanup-after.json` and
`scratch-prune.log` record the cleanup. The baseline exception and six-script
dependency hashes were verified afterward; the scratch path is absent.
No active Task W process, mount or private cluster remains. Nothing was merged
or pushed.
