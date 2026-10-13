# W100 EOF rounding: missing REPORT size record — reproduction and fix verdict

**Verdict: PASS** (fix gated on the owned two-host fixture; baseline
reproduction retained on the same harness).

**Purpose.** A REPORT publishes chunk mappings together with per-inode
logical sizes. When a chunk record arrived without its inode's size record
(reachable when the client's staged inode lookup fails between the chunk
mark and the ino mark), the server guessed EOF as the chunk end
(`(ci+1) * EFS_MIN_CHUNK_SIZE`, `src/server/raft_host.c` report loop).
The guessed extent commits to the grow-only lane MAX, so no later report
can shrink it: a 512-byte file read back with EOF 131072 through an
independent mount. Historical incident: ecopy's mpfr `Makefile.in`,
32398 bytes on XFS vs 131072 on efs (2026-09-30, ecopy.strace).

**Tested commits.**

- Baseline (bug retained): `fc341f00` + uncommitted W100 fault hook,
  build identity `efsd fc341f00-w100-fault (branch devel, built
  2026-10-13T22:30:40Z on intel1)`, deployed at
  `/data1/efs/w100-private/baseline`.
- Fixed: `1302ec58b` on `devel-kimi` (single commit on `fc341f00`),
  `efsd v0.3.2-pre-alpha-336-g1302ec58b (branch devel-kimi, built
  2026-10-13T23:29:55Z on intel1)` / `…T23:32:08Z on intel2`, deployed at
  `/data1/efs/w100-private/devel-kimi`. SHA256 (intel1 build):
  efsd `84127d968c3092118fb7bb50890a8e05ddbdc7ef29305ea0e751e5a5dd6ec5e8`,
  efs-fuse `b94a778b1e3b6392481a69b124192b5223fcfdafa5990c1b25824ab22e8df30a`.

**Fix.** A chunk record without a size record on a *live* inode is
malformed: the report loop rejects the batch with `EFS_ERR_INVAL`
(phase `pack-nosize`, logged per record). The client retains its dirty
data, fsync fails with EIO, and the next REPORT (with the size record)
publishes exact bytes. A *deleted* inode is not malformed — publish
skips it as before (`NOT_FOUND` is answered before size is consulted).
Zero remains an explicit legal EOF; wire format unchanged. Client change
is limited to the warning text; the `extra_cap` size-record reservation
in `src/client/write.c` already covers the mark-ordering race.

**Environment.** Intel1 (`10.0.0.207`) + Intel2 (`10.0.0.209`), Core
Ultra 7 256V, owned four-daemon fixture on ports 27432/27433 (2 GiB
quota per node, files engine, direct I/O, TCP), per
`scripts/intel_w100_private.py`; features `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`, `EFS_EXPIRY=0`,
`EFS_MD_RAFT_N=4`. Fault client env `EFS_FAULT_REPORT_SIZE_FILE` (the
`OMIT_SIZE <ino>` hook drops one size record after valid construction).
Peer reads via Intel2's independent mount. Production stores and the
main Intel cluster (ports 17432/17433) untouched.

**Reproduction.**

```sh
python3 scripts/intel_w100_private.py stop rounding-baseline baseline   # if running
python3 scripts/intel_w100_private.py start <run> <source-label>
ssh intel1_efs python3 tests/live/report_eof_contract.py \
    /data1/efs/w100-private/runtime-<run>/mnt \
    /data1/efs/w100-private/runtime-<run>/omit-size --expect rounded|refused
```

Cleanup: `python3 scripts/intel_w100_private.py stop <run> <source-label>`;
fixture storage under `/data{1,2}/efs/w100-private-<run>/` may then be
deleted. The contract test requires the `intel2_efs` alias to resolve
*on Intel1* (provisioned 2026-10-13: intel1 efs key in intel2's
authorized_keys, `~/.ssh/config` entry).

**Results.**

- Baseline, `--expect rounded`: PASS — `small` (512 B written) read back
  EOF 131072 with a zero at offset 512; `sparse` (8 MiB+1) read back EOF
  8519680. fsync succeeded: silent corruption confirmed.
- Fixed, `--expect refused`: PASS — fsync under the fault fails EIO;
  peer observes size 0 (nothing fabricated); after the fault clears, a
  repeated fsync publishes exact EOF 512 / 8388609 with correct payload
  and holes (`48`/`4c`). Server log shows
  `REPORT rec ino=<n> ci=<k> has no size record; rejecting batch`.
- Fixed build, full `tests/posix/posix_suite.py` on the fixture mount:
  216/216 pass, 0 fail, 0 skip (31.9 s).

**Limitations / remaining gates.** This gates the EOF contract only; it
is not the W100 aggregate repair (concurrent POSIX timeouts and
mixed-workload EIO stay open per the work item). The 2-client peer suite
and persist suite were not run on this fixture. An fsync that fails EIO
in the (rare) staged-lookup window is the intended visible failure; a
client-side deferral of sizeless reports would soften that and is
deliberately not part of this change.
