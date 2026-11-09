# FIN accounting — strict zero-application-TCP audit

**FIXED (scoped, branch only)** on `agent/devel-k35-slow-fsync`, product
`38f3cecba` (parent `b4d512ace`). Strict hardware TCP/RDMA audit PASS,
785/785 production receipts measured, application TCP 0/0, violations 0.
Full gate 546 / 0 FAIL / 13 NOTRUN, INCOMPLETE for disclosed profile/host limits.
Original failed audit and all fixtures retained unchanged. No merge/push.

The [isolated prerequisite follow-up](fin-gate-notrun-closure-20261029.md) now
passes the full gate: **546 / 0 FAIL / 0 NOTRUN** at `c571b862f`. The earlier
TCP-profile incomplete result and its original evidence remain historical.

## Failure and accounting rule

The [K35 qualification](k35-slow-fsync-20261029.md) passed durability but its
strict raw-zero audit failed: 863 measured RDMA connection receipts, TCP tx 0 /
rx 18, violations 0. Every nonzero receive delta was one octet. Source
subtracted FIN only for TCP_CLOSE_WAIT (8); a no-payload control demonstrated
that receive sequence accounting retains FIN after both endpoints close (7).
Closed state alone also follows reset, so subtracting for every state 7 would
hide a real one-byte payload. That shortcut is not used.

The actual production helper `src/common/tcp_counts.h` normalizes cumulative
TCP_INFO counters using positive FIN evidence: Linux TCP_CM_INQ includes the
FIN sequence octet while FIONREAD excludes it. A non-consuming one-byte peek
obtains the ancillary hint; the queue-count difference must be exactly one.
SO_OOBINLINE makes both queue observations include urgent payload, preventing
an urgent byte from masquerading as FIN. Unchanged cumulative send/receive
counters bracket the observations; at most three immediate samples are made.
Unsupported or inconsistent metadata observations remain unmeasured and fail
the strict audit. EAGAIN/reset is handled conservatively by retaining every
raw receive octet, never by authorizing a FIN discount. Cumulative
counters still detect payload already consumed by a reader; this is not a
replacement with unread queue length. No application read/write, retry,
deadline, durability rule or transport-violation check is relaxed. A peek at
teardown may observe/clear a pending reset error; it never discards payload.
The log now records TCP state and the positive FIN verdict.

Kernel reference: [Linux v5.14 TCP receive ancillary hint](https://raw.githubusercontent.com/torvalds/linux/v5.14/net/ipv4/tcp.c)
(`tcp_inq_hint`, `tcp_recvmsg`) and its FIONREAD implementation. These are
source semantics, not evidence that a particular old connection received FIN;
original connection states were not captured and that limitation stays.

## Regression and qualification

`tests/test_tcp_counts.c` runs the actual helper on fresh loopback sockets,
without an RDMA device. It checks zero payload, one sent byte, one already
consumed byte, FIN with payload, both endpoints closed, local SHUT_RD without
FIN, RST with a queued byte, the byte consumed after reset, urgent payload
plus FIN, transmit counting and invalid-descriptor refusal. Every one-byte
negative must remain one byte. It is in `make test` (therefore `test-all`),
with explicit rebuild dependencies for the test and RDMA object.
The first zero-length-peek diagnostic could not obtain an ancillary hint and
failed its regression; logs and source trees remain; no core file is used as evidence. The corrected one-byte peek
passes. These development failures are not erased by qualification.

Product `38f3cecba` is exported with `git archive 38f3cecba` into new private
trees only. Reproduction must choose fresh unused source/evidence suffixes
matching the guards; never reuse any retained fixture or evidence directory. Intel2's later
preflight detected another agent's pump measurement and stopped before any
gate command; its log stays at
intel2:/home/maik/efs-artifacts/fin-fullgate-20261029-38f3cecba-preflight.log.
Intel1/xorinox have Task T work and real clients/stores; fstor007 has Task T.
No foreign process, client, mount or store was operated. Source transfer
preceded the rejected gate launch; a staging/measurement overlap on intel2
is not ruled out and no clean-run claim is made for that attempt. Node9900 has only the pinned real client
PID 2380112/start ticks 353877425; its exact argv/identity is revalidated,
never operated. No shared-client run is called quiet-host performance proof.

Hardware source: /tmp/efs-k35-slow-fin38f/src; raw:
node9900:/data1/efs/evidence/k35-slow-fin-20261029-38f3cecba.
Private FUSE 3.16.2 /data1/efs/deps/libfuse/lib64, active mlx5_0 InfiniBand;
Linux 5.14.0-687.39.1.el9_8 x86_64. Fresh 3/4/5-member same-host fixtures,
packing/rebuild enabled, buffered FUSE I/O; workload, quotas and all bounds
are exactly the [prior K35 protocol](k35-slow-fsync-20261029.md#hosts-and-reproduction).
Existing KEEP flags retain every related fixture. No preload/GID override.

```sh
# External wrappers from 690169143/217007986; source export stays 38f3cecba.
bash scripts/evidence/fin/fin_accounting_hardware.sh /tmp/efs-k35-slow-fin38f/src \
  /data1/efs/evidence/k35-slow-fin-20261029-38f3cecba 38f3cecba
# Requires closed successful hardware/audit receipts and two new host guards.
bash scripts/evidence/fin/fin_accounting_gate.sh /tmp/efs-k35-slow-fin38f/src \
  /data1/efs/evidence/fin-fullgate-node9900-20261029-38f3cecba 38f3cecba \
  /data1/efs/evidence/k35-slow-fin-20261029-38f3cecba
```

The hardware driver reruns eight native RDMA modes including intentional
one-byte TCP violation, B6 slow and K35 over both TCP/RDMA, then declaration
TCP/RDMA and `scripts/evidence/fin/fin_accounting_audit.py`. The strict audit requires **every**
production RDMA receipt measured, positive application RDMA totals, zero
post-setup TCP tx/rx and zero violations. Missing fields/measurements fail;
no receive-delta exception. Intentional native TCP-violation evidence is
reviewed separately and must still show a positive violation count. TCP
fixture logs must have no RDMA connections. Full `test-all` is unchanged
apart from adding the new socket regression; eight build/test jobs.

## Closed hardware review

Hardware/controller, all eight native commands, declaration TCP/RDMA, TCP/RDMA
B6 slow and K35, exact candidate socket regression and strict audit each exit 0.
No native NOTRUN/SKIP. The controlled native one-byte TCP violation still
records violations > 0 and retires the connection; it is not part of the
positive fixture audit. All original native deadline/interruption/burst/churn
checks pass. B6 acknowledges 22 exact files per transport through restoration
and whole-cluster restart, no cleanup errors or refused files; three required
fsync marks in each. TCP K35: 145.1 s / 317 exact files; RDMA: 170.3 s / 486.
Two-down EIO for fsync/fdatasync, rebuild, 4/5-member placement, leader-stop
and death-during-COMMIT assertions remain intact.

Strict production receipts: TCP fixture has no RDMA connections. RDMA has
**785/785 measured**, application-byte endpoint totals tx 684563386 /
rx 682869674, **post-setup TCP 0/0, violations 0**; no bad receipt and no
allowance for one-byte deltas. Positive FIN evidence in 408 receipts: 397
CLOSE_WAIT (8) and 11 CLOSE (7); other 377 are established (1). Deliberately
killed endpoints may not emit teardown counters; this is every retained
production receipt, not an invented receipt for a crashed endpoint.
All 70 ownership samples have no foreign test/build job and retain the
pinned real client; this remains shared-client functional coverage.
The full gate runs separately after these receipts close, using the same
source and private fixture base; historical captures remain separate.

## Full gate and distinguishing controls

The separate node9900 full gate finished in 668 s plus 10 s build, eight test
and build jobs: **546 / 0 FAIL / 13 NOTRUN**, controller/gate exit 3. All 546
command exits are zero; the new real-socket regression is command 303 and
passes. NOTRUNs: absent cluster-stop/remount scripts, absent Valgrind (RDMA
Memcheck and leak fixtures), sanitizer attach refusal, and nine RDMA paths
suppressed by this gate's explicit TCP profile. The HCA is available: all
nine supplemental native/declaration checks ran over mlx5_0 and passed.
These supplemental results do not erase the full gate's NOTRUN receipts.
No full release PASS is claimed.

Seven advisory over-budget results remain: K35 153.9/80 s, stop progress
53.3/50, member limit 36.1/20, coordinator kill 53.5/25, D39 model shards
34.8/25, 18.6/15 and 6.3/5. No budget or deadline was changed. All four B6
modes pass; down/refuse/slow each acknowledge 22 exact files, full 48, with
empty cleanup-error arrays. Slow F0 fsync succeeds at 60.03 s with proof;
other fsyncs 8.00–8.05 s. Full mode's W0/W1/W2/fill-29 ENOSPC are required
negative oracles, never counted as successful writes. Full-gate ownership:
68 samples, no foreign test/build jobs, only the verified real client.
Final check finds no private daemon/client or private mount; the real client
still has PID/start identity unchanged.

Hardware and full gate use identical binaries: `efsd 38f3cecba`, branch
`agent/devel-k35-slow-fsync`, built 2026-10-29T01:52:07Z on node9900.
efsd SHA256 `9d483edc22d1506d0b82cf0e1a9bccf197d8fa22f8b31ae7878bda08cd9d075e`,
efs-fuse `6377701587e96152198760dfa522e69cc0034f9211ca2245b7b56d48ab46a525`;
rdma.c `de63ce5b945c8797059c24548bff9fdeb263ec6d2a720adc4d0e5ed6e1de719b`,
tcp_counts.h `d3ab67e6330830d9787b96646f60325fdf75e1b64ad719b4310143333861af5e`.
All binary/source/build/environment receipts remain in the raw captures.

After both protocols closed, `fin_accounting_controls.py` compiled the actual
candidate helper, the exact old `tcp_counts` body, and the unsafe variant
that also discounts state 7. Candidate exits 0 with all 15 positive checks;
old and unsafe variants SIGABRT as expected after respectively 4 and 10
positive-prefix checks. The driver verifies the accounting assertion and
prefix counts, not just any crash. This pins the FIN failure and the later
one-byte-reset failure separately. Core suppression applies only to these
expected-abort child controls. No production source/test changed after the
qualified commit.

```sh
git show b4d512ace:src/common/rdma.c > /tmp/fin-old-rdma.c
python3 scripts/evidence/fin/fin_accounting_controls.py /tmp/efs-k35-slow-fin38f/src \
  /tmp/fin-old-rdma.c /data1/efs/evidence/fin-controls-FRESH
```

## Preservation and closed evidence

Do not remove the failed audit, its 18 nonzero receipt lines, FIN control,
or any old/new fixtures. Original audit capture (89 files, manifest SHA256
`adb2166e6436b5fce38e960027ce1d98c5e527a1037f4ab274a3f254d0feabda`):
node9900:/data1/efs/evidence/k35-slow-node9900-20261029-1ab9fef83.
The original two K35 failures remain at the exact xorinox paths on the
[K35 evidence page](k35-slow-fsync-20261029.md#preservation-evidence-and-limits).
Development raw logs: node9900:/data1/efs/evidence/fin-dev-20261029;
private trees /tmp/efs-fin-dev and /tmp/efs-fin-dev-v2 remain for investigation.
All K35 successful fixtures/builds are kept pending open acceptance/merge
review as previously recorded. No bulk is deleted in this follow-up.

All four new closed captures were hashed and `sha256sum -c` verified on
origin. The original failed audit's entire manifest also re-verifies unchanged.
Both original xorinox failures still have all three stores and daemon logs.
No bulk, fixture, payload or evidence was removed. Raw artifacts stay outside
Git. Source, tests and Makefile remain byte-identical to `38f3cecba`; later
commits contain reproduction drivers, evidence and required graph/docs output.

All new directories below are on node9900 under /data1/efs/evidence/.

| Capture directory | Files | SHA256SUMS SHA256 |
|---|---:|---|
| k35-slow-fin-20261029-38f3cecba | 90 | 7cd33c2f5f8fca3d0bef3e9cfa4cafa9bb7d2964b14ab8090a42db5b40824370 |
| fin-fullgate-node9900-20261029-38f3cecba | 80 | 3627fee48b4d2fb96b4fe297cd245de32f45de70f7ee1464a1243992e4a86cc3 |
| fin-controls-20261029-38f3cecba | 17 | ebfc2a031e70dbb9809cdea870b7bdd291555031dd5c39a68017937c51b53008 |
| fin-dev-20261029 | 17 | aaf5eec20aaf402ac954038379bad6512b37908be67074c3896f4641c9858a7c |

Intel2's rejected preflight file SHA256 is
`1cc25e80679a7a437bca141b71e0745fcefa7af60fa4c64b7b287f6e63f51c6a`.
Fresh TCP fixtures: efs-b6-ksvo4bbq / efs-k35-ekzgw4v1; RDMA:
efs-b6-v2e09mfg / efs-k35-u3i7qg7b, all in the new source's tmp/.
Full-gate B6/K35 fixtures remain under its tmp/case-* roots, with exact paths
in JSON and copied fixture-log names. The two earlier K35 failures and every
older hardware/gate fixture remain in their original trees.

Documentation generation/all five checks PASS; canonical graph rebuild and
nine navigation tests PASS (16,816 nodes / 69,601 edges). The protocol.h
partial-parse warning, 49 files without extracted symbols and 1,473 unresolved
calls remain navigation coverage limits, not runtime evidence. Full multi-host/release acceptance remains
separate; thirteen NOTRUNs are not passing tests.
