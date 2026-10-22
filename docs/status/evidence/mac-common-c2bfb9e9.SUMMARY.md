# mac-common-c2bfb9e9/ — evidence summary

## Purpose and linked item
Compact proof files and sanitized logs for the manager-authorized
defect-only qualification of the common candidate
`c2bfb9e9a281aae33dfaa01689e61866ccf60af8` on the Mac host. Files were
added across commits `b71c5317f`, `c3f0f9b43` and `f3359da53`. Linked
status page: `docs/status/mac-common-candidate-c2bfb9e9.md`.

## Verdict
Mixed, per gate (never upgrade — the linked page's stop checkpoint is
authoritative):

| Gate | Result |
|---|---|
| Bounded native qualification + ASan/UBSan audit (c2bfb9e9) | PASS |
| Full `make test` normal (c2bfb9e9) | PASS, exit 0 |
| Full `make test` fault, EFS_FAULTS=1 (c2bfb9e9) | PASS, exit 0 |
| Original mounted buffered/TCP POSIX fixture | FAIL: 199/216 cases, 17 timeouts |
| 410v2 full native gate | STOPPED exit 143 on manager instruction (not PASS) |
| 410v2 fault suite and new mounted gates | NOTRUN |

## Source commit and build identity
Candidate `c2bfb9e9a281aae33dfaa01689e61866ccf60af8`; archive sha256
`4144442e…d6638ec`, 1557-input manifest sha256 `e3eff745…a1a64f2`,
52-directory manifest sha256 `272154ba…a3110` (per linked page).
Per-gate binary sha256 identities for `efsd`, `efs-fuse`, `efs-mgmt`,
`libefs.a` are recorded in `mac-full-native-proof.json` and
`mac-full-fault-proof.json`; sanitized-build identities in
`mac-qualification-proof.json`. Stop proof records source
`410afed433b6da093bdc2acf37bec253ddbd586c` with 1563 verified inputs.

## Environment
Mac host efs1, private directory `/tmp/efs-mac-c2bfb9e9-y2ZaN2`, private
TMPDIR/EFS_TEST_TMPDIR, disk-backed build trees, Python 3.13.13.
Installed service left active at 192.168.2.11:17432 (PID 165163,
buffered, 10G quota); installed stores untouched.

## Method
Verify all 1557 file hashes and 52 directories before build; run
isolated real-LSM audit, preflight/capture/sender C controls and full
`make test` in normal and EFS_FAULTS=1 modes with original
tests/budgets; reverify all inputs unchanged afterwards. Mounted
fixture used manager-approved buffered/TCP query_publication
(RPC/FUSE/POLICY/POSIX/FAILOVER), 1G private quota, original
15/30/75-second case budgets and 360-second outer budget. Raw
posix.tsv/client-stop.log are external, not in this directory (see
`full-gate-raw-identities.json` for the external path and hashes).

## Results
- Both full suites exit 0; all 1557 inputs / 52 directories reverified
  unchanged after each mode.
- Mounted fixture: 199/216 pass, 17 timeouts, 0 skip/notrun in 184.2s;
  both private mounts drained cleanly; cause of the timeouts not
  established.
- 410v2: native STOPPED (exit 143), fault/mounted NOTRUN; 1563 inputs
  unchanged; no owned processes or mounts left.

## Limitations and remaining gates
This directory holds compact proofs and sanitized excerpts, not full
logs; full raw logs are external (`results/mac-readindex-resume/
c2bfb9e9-full`, hashed in `full-gate-raw-identities.json`). Server logs
show lane-sweep BUSY/apply waits/GC work without establishing the
timeout cause. Remaining: mounted 17-timeout root cause, 410v2 full
prerequisites and mounted/cold acceptance, retained orphan inode4091
ownership. No unchanged broad mounted retry authorized.

## Evidence index
- `mac-common-c2bfb9e9/mac-full-native-proof.json` sha256:35998ac19b87e68d — full normal suite PASS
- `mac-common-c2bfb9e9/mac-full-fault-proof.json` sha256:6461bd38c1faa684 — full fault suite PASS
- `mac-common-c2bfb9e9/mac-qualification-proof.json` sha256:b55e57e31598866a — bounded + ASan/UBSan audit PASS
- `mac-common-c2bfb9e9/mac-410-stop-proof.json` sha256:8a207a5d8bbb2bff — 410v2 STOPPED/NOTRUN checkpoint
- `mac-common-c2bfb9e9/full-gate-raw-identities.json` sha256:3b5252d4f05be390 — external raw log path + hashes
- `mac-common-c2bfb9e9/mac-audit-sanitized.log` sha256:e315745decd18b19 — sanitized audit excerpts
- `mac-common-c2bfb9e9/mac-capture-sanitized.log` sha256:28f7255e2c56273d — capture control excerpts
- `mac-common-c2bfb9e9/mac-preflight-sanitized.log` sha256:bb253bf10b4c4b2a — retained-orphan preflight excerpts
- `mac-common-c2bfb9e9/mac-sender-sanitized.log` sha256:0f2b4c070b128275 — sender control excerpts
