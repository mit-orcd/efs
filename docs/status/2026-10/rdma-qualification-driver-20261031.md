# RDMA live qualification driver, October 31

**Purpose:** let `scripts/qualify.py` run the append, torn-read, GC and listing harnesses over RDMA, so the RDMA half of the [Stage 1 acceptance matrix](../stage1-acceptance-matrix-20261030.md) rows that name those harnesses can be scheduled (that page, "What the campaign cannot cover", item 3). Task BS. **Verdict: PASS**: all four harnesses completed over RDMA on fstor007, each with a TCP control on the same host. This is a driver and harness change plus a short proof run per harness; it is not a K-row verdict and does not close any row.

**Tested source:** `94e82c731` on branch `claude/bs-rdma-qualify-1405de` (driver and harness changes only; product code is identical to its parent `909a07d71`). Binaries built on fstor007 from `git archive 94e82c731` with `EFS_GIT_ID=94e82c731`: `efsd` sha256 `e93d86b9…`, `efs-fuse` `102b3367…`, `efs-mgmt` `58478bdd…`, fault-build `tests/faults/efsd` `309daec4…` (full values in `env.txt` of the artifacts). `efs-fuse` is linked against a private libfuse 3.16.2 (the system has 3.10.2; the Makefile needs 3.12) taken from the retained Task L dependency copy.

## What the driver lacked

1. **Transport selection.** `append_failover.py` and `k14_atomic_cluster.sh` hard-coded `EFS_TRANSPORT=tcp`; `qualify.py` passed no transport flag to any live harness, whatever `cluster.transport` said, so an `rdma` description still ran the harnesses over TCP and labelled nothing. `gc_reclamation.py` and `qualify_ao_readdir.py` already had `--transport`, but the driver never passed it.
2. **Device naming.** No gate exported `EFS_RDMA_DEV`. The daemons find a device themselves, but `gc_reclamation.py` defaults to `rxe0`, which does not exist on an InfiniBand host.
3. **The listing harness was not a driver gate at all.** `scripts/evidence/ao/qualify_ao_readdir.py` (K74) was run by hand.
4. **Addresses and ports.** None needed: the harnesses start private clusters on `127.0.0.1` and TCP carries RDMA setup (`src/common/rdma.c`, "rdma on mlx5_0 (TCP carries setup)"), so the address format and port ranges are unchanged. `qualify_ao_readdir.py --base-port` accepts only 19000-19996; the new gate leaves ports to the harness's free-port picker unless `fixture.ao_base_port` is set.
5. **Timeouts.** None needed: the 3600 s gate deadlines are unchanged and RDMA runs were not slower.
6. **Result labelling.** Gate results carried no transport. A silent TCP fallback was possible in `append_failover.py` and `k14_atomic_cluster.sh`, which had no check.
7. **Loopback driver runs** (`--loopback N`) hard-coded TCP and offered no way to set a harness's fixture, so a short run needed a hand-written description.

## The change

- `scripts/qualify.py`: `live_gate(build, rdma=False)`. For `append-failover`, `k14-torn-read`, `gc-reclamation` and the new `ao-readdir` gate (`rdma=True`), `cluster.transport == "rdma"` selects RDMA: the gate needs `cluster.rdma_dev` (NOTRUN if unset), checks `/sys/class/infiniband/DEV` on the private host (NOTRUN if absent, never a silent TCP run), exports `EFS_RDMA_DEV`, and adds `--transport rdma` / `K14_TRANSPORT=rdma`. Every gate result's `fixture` now carries `transport` (and `rdma_dev`); gates that do not take a transport (`k13-tickets`, `fragment-integrity`, `d5-expiry`) stay TCP and say so. With `transport` tcp, unset or auto the commands are unchanged (the self-check compares them). Two small run options: `--rdma-dev DEV` (with `--loopback`: daemons, mounts and harnesses over RDMA) and `--fixture KEY=VALUE` (override a fixture value, e.g. a short `append_failover_args`). The summary draft's limitation line no longer claims "TCP loopback only".
- `tests/live/append_failover.py`: `--transport tcp|rdma` (default tcp), and a verdict check that every `efsd` and client log shows `transport rdma: rdma on` (`efs_transport_startup` refuses to start rather than fall back, so the line proves the mode).
- `scripts/evidence/k14/k14_atomic_cluster.sh`: `K14_TRANSPORT` (default tcp) and the same startup-line check as a `check` row.
- `scripts/qualify_matrix.json`: `ao-readdir` added beside `gc-reclamation` in the two gate lists; the two "RDMA not driven" notes replaced.
- Self-check `tests/test_qualify_rdma_live.py` (in `make test`, about 0.1 s, no budget row; `tests/test_make_test_recipe.py` passes, budgeted gate 1733 s of 1750 s): runs the gate against a fake runner for all four builders on tcp and rdma, NOTRUN for a missing or absent device, TCP labelling of non-RDMA harnesses, and `bash -n` of the script. The RDMA runs themselves are NOTRUN there. `tests/test_qualify_driver.py` passes on fstor007 (it needs `/proc`; it cannot run on the Mac).

## Environment

fstor007 (ORCD, 96 CPUs, 754 GiB), `mlx5_0` (ConnectX-7, InfiniBand, port 1 active, MTU 4096), RHEL 9.7 (kernel 5.14.0-687.36.1.el9_8), Python 3.9.25, GCC 11.5; 3 `efsd` plus two clients per run on `127.0.0.1` (driver loopback cluster on ports 18740-18742, private harness clusters from base 18700, harness-chosen free ports for `ao-readdir`; no other listener on 18xxx/19xxx beforehand), `O_DIRECT` fragment I/O, stores under `/tmp/efs-taskBS-work`. No other session's daemons were on 18xxx/19xxx. Single host, one HCA, loopback: this is **not** a distinct-host or multi-HCA qualification. The clusters here are the harnesses' own private ones; no live cluster, user mount or `/data1/efs` path was touched.

## The eight runs

Prerequisites: tree shipped with `git archive`, built with `make efsd efs-fuse efs-mgmt` and `make tests/faults/efsd` (the listing harness uses the fault build), `EFS_FUSE_PREFIX`, `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` pointing at libfuse >= 3.12. Each run is the driver with one gate, for example:

```sh
python3 scripts/qualify.py run --loopback 3 --candidate 94e82c731 --gates k14-torn-read \
  --deadline k14-torn-read=1500 --out OUT --work WORK --rdma-dev mlx5_0 --fixture 'k14_phases=1 3'
```

Omit `--rdma-dev` for the TCP control. The fixture overrides: `append_failover_args=--records 2000 --writers 4 --failover step-down --interval 4 --first-event 3`, `k14_phases=1 3` (steady rewrites, then writer client SIGKILL and remount), `gc_args=` (no `--posix`), `ao_args=` (no `--posix`). The script is `runall.sh` / `runao.sh` in the artifacts (see below); cleanup: the driver deletes `WORK` at the end of each run.

| Harness | Transport | Verdict | Gate time | Run total | Result lines |
| --- | --- | --- | ---: | ---: | --- |
| `append-failover` | RDMA | PASS | 52.5 s | 60 s | verdict PASS, failover exercised, `started_as_requested` true (harness wall 21.9 s) |
| `append-failover` | TCP | PASS | 83.6 s | 91 s | verdict PASS, failover exercised (harness wall 51.6 s) |
| `k14-torn-read` | RDMA | PASS | 87.6 s | 94 s | p1 153,077 reads, 0 torn; p3 199,840 reads, 0 torn; `PASS rdma: every efsd and client started in rdma mode`; `RESULT fails=0` |
| `k14-torn-read` | TCP | PASS | 87.9 s | 95 s | p1 172,120 reads, 0 torn; p3 225,197 reads, 0 torn; `RESULT fails=0` |
| `gc-reclamation` | RDMA | PASS | 51.7 s | 59 s | 71 PASS marks, the harness's per-process verbs-descriptor proof passed |
| `gc-reclamation` | TCP | PASS | 52.2 s | 59 s | 71 PASS marks |
| `ao-readdir` | RDMA | PASS | 25.4 s | 33 s | 6 `AO PASS` checks, the harness's RDMA runtime evidence check passed |
| `ao-readdir` | TCP | PASS | 7.3 s | 15 s | 6 `AO PASS` checks |

Run totals include the driver's three-daemon loopback cluster start and stop. The driver's overall exit code in every run is 3 (NOTRUN): only one gate was selected, so the identity ledger rows are NOTRUN; each gate's own verdict is the PASS shown. These are short control runs, not the full gates: `append-failover` used 2,000 records instead of 8,000 (matrix: 19 min on nuc), `k14` phases 1 and 3 of `1 2 3 7`, `gc-reclamation` without `--posix`, `ao-readdir` without `--posix`.

**Differences in outcome:** none in verdict. Over RDMA `append-failover` finished in 21.9 s against 51.6 s (harness wall) for the same 2,000 records and failover schedule; `ao-readdir` took longer over RDMA (25.4 s against 7.3 s), one run cannot separate this from noise or explain it. `k14` and `gc` times match within 1%. No RDMA correctness failure was seen in efsd or the client, so no K-row candidate arises.

A first attempt at `ao-readdir` (both transports) failed in 7-8 s with `FileNotFoundError: tests/faults/efsd`: the harness needs the fault build, which I had not made. It ran no harness code; the failed attempts are kept in the artifacts as `ao-readdir-*.norun-nofaultbuild` and the two runs in the table were made after `make tests/faults/efsd`. `qualify_ao_readdir.py` documents this requirement only through `k33_daemons.py`; deploys that run the `ao-readdir` gate must build it.

## Raw evidence

On fstor007 `/tmp/efs-taskBS-artifacts/` and on the Mac `~/efs-artifacts/taskBS-20261031/` (460 files, 5.0 MB, `SHA256SUMS` sha256 `38f6244ecca6dd1568e3b67798165cbb5ec63147dc23a1c32be0152092df1e14`; the copy was re-verified against the sums). Contains the per-run driver output directories, console logs, `runs.txt`, build logs, `env.txt` (host, `ibv_devinfo`, binary hashes) and the two run scripts. The shipped tree, libfuse copy and work directories on fstor007 were deleted with `edelete`. Nothing raw is committed.

## Limitations and remaining gates

- Single host, loopback addresses, one HCA: no distinct-host RDMA, no cross-HCA, no multi-node failure domains. The D-RDMA batch still needs its cells.
- One short run per harness and transport. The full-length gates (append 8,000 records, all K14 phases incl. the `EFS_FAULTS` phases, `gc` with `--posix`, `ao` with `--posix`) were not run; K14 phases 4-6 also need the `faults_source` fault build.
- `k13-tickets`, `fragment-integrity` and `d5-expiry` still run on TCP only (D5 expiry asserts TCP).
- The RDMA proof for `append-failover` and `k14` is the startup line of every process; `gc` and `ao` additionally check for verbs descriptors or runtime evidence.
- The `rdma_dev` key must be set in any cluster description that sets `transport: rdma` and runs these gates; the matrix topologies for `orcd-mini-rdma` and the fcstor cells do not set it yet (they would be NOTRUN for these four gates until they do).
