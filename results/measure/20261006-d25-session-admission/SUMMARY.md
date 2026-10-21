# D25 I23 session admission checkpoint (20261006)

## Purpose and linked item

Checkpoint for D25 I23 — publication session admission/fencing, tracked in
`docs/status/d25-admission-routing.md` ("I23 publication session admission —
implemented, staged"). Implemented in `fc29cfd7`; management leader routing
in `04f267a0`. Added by commit
`dec90f576 docs: record I23 admission and four-node NUC validation`.

## Verdict

PASS — full Linux unit suite PASS; live publication session gates PASS
(same-group and cross-group session authorities); final rollout on clean
`04f267a0`: POSIX 216/217 pass, 0 fail, 1 skip (unsupported mmap), 0 notrun
in 48.7 s; all four servers match, cluster status healthy. D25 itself
remains **staged**, not activated (see Limitations).

## Source commit and build identity

Deployed build (from `rollout.log` compile lines): commit `04f267a0`,
build id `04f267a03a24`, version `v0.1.0-pre-alpha-89-g04f267a0`, branch
`devel`, build time 2026-10-07T01:36:02Z, gcc `-O3 -march=native`.
Feature commit `fc29cfd7` named in the original summary. Installed
management binary UUIDs `e009290432019a13df7b71f0eed61df0` and
`873625e6dbf56f2fe3ae2177a6746f75`.

## Environment

Four-node NUC cluster (`nuc_efs`, nodes n1–n4), deployed by rsync + remote
build via devops scripts; efs-fuse mounted at `/data1/efs/mnt` (500 G
quota, ~1 % used). Transport/features not explicitly recorded in the run;
buffered I/O (no direct-I/O flags in the rollout log).

## Method

1. Full Linux unit suite on the build host (`unit.log`, all suites OK).
2. Live publication session probes (`live.log`): disposable empty files and
   invalid FileIDs, no fragment PUTs; same-group and cross-group session
   authorities; probe files removed afterwards; small
   session/receipt/floor records intentionally retained.
3. Rollout (`rollout.log`): stop n1–n4 → rsync source to
   `nuc_efs:/data1/efs/src` → remote build → start → smoke → POSIX suite →
   cluster status.

## Results

| check | result |
|---|---|
| Linux unit suite | PASS (all suites OK) |
| Live publication session gates | PASS (e.g. gate PASS uuid=b2a4c992…, shard=1, lane_group=0, session_group=2); fenced ESTABLISH correctly rejected (`rc=-13`) pre-fence, accepted after fence/register |
| Rollout POSIX suite | 216/217 pass, 0 fail, 1 skip (mmap unsupported), 0 notrun, 48.7 s |
| Cluster status post-rollout | all 4 nodes match, healthy, idle |

Behavior admitted by the checkpoint: serialized apply rejects fenced
submissions and retirement before touching receipts; a transport STALE
retains UNKNOWN client ownership; production ESTABLISH requires an
authoritative ACTIVE epoch and registered shard; the management session
coordinator follows advertised leaders per shard with bounded redirects and
no ambiguous transport retry. Reclaim eligibility requires both the
completed global barrier and a durable local rejection floor.

## Limitations and remaining gates

- D25 remains staged: receipt/floor deletion and the cleanup sweeper are
  not implemented.
- Next before activation: coherent lane-local mtime invalidation,
  automatic FUSE lane admission, both flush integrations, and bounded
  abandoned-stream cleanup.

## Evidence index

- `unit.log` (7fc45eb652af2448) — full unit suite, all OK
- `live.log` (79c898132e85fa9c) — live session probes incl. fence rejection and gate PASS lines
- `rollout.log` (5d90dfc1fcb428de) — deploy/build/POSIX rollout, 216-pass line, build identity
