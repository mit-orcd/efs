# orcd-mini deployment of devel `09f8af09c` — 2026-10-16

**Deployed. Live suites PASS (POSIX 216/216, two-client 67/67, durability
27/27). Unit suite PASS except 4 environmental failures. Crash-path gates
FAIL on a known pre-existing gap:** private GC gate and fault-suite
integration (4/4) fail on the permanent uncertain-recovery fence from
`c4178ea81` (W71 recovery blocker). The previous deployment `4d6530dd8`
fails the same GC step.

Linked items: the group A removal ([status index](README.md)), the RDMA
credit fix ([orcd-mini RDMA credit](orcd-mini-rdma-credit-20261016.md), now
deployed), and W71 ([AMD async durability](amd-async-durability-20261015.md#review-follow-up-clean-restart-versus-uncertain-recovery)).

## Build and deployment

- Source: devel `09f8af09c2be` (group A removal on top of `d57da8af6`;
  includes `e84d42cbb`/`bbebedea8`). It is built from a clean `git archive`
  with build ID `09f8af09c2be`, version `v0.3.2-pre-alpha-573-g09f8af09c`
  and build time 2026-10-16T20:51:35Z.
- node9901 (RHEL 9.7) `/data1/efs/releases/09f8af09c2be`:
  - efsd `6dbd266786105793d9f9442ddee80e43f8e261cdf2e4a25297acc31e9c1f9fab`
  - efs-fuse `e29ee49e57eb291dacd42a240968bca6c4ba292f6954ba36ca590d79c3a8fe00`
  - efs-mgmt `d0beb91d0a6501f65944c50469ef35821d885c9f1248e237776d7016cd2e9f1d`
- node9900 (RHEL 9.8) `/data1/efs/orcd-mini-client/releases/09f8af09c2be`:
  - efs-fuse `cb6ca44a443363dd2787fadb0c3b28c5476673f83410a82e4787db4a67e23417`
  - efs-mgmt `842b1f760209a1d6beab5534834c986891d52f7791ed91e6893449bf5717fb5c`
- Before the switch, both mounts had no visible users and the daemons were
  idle (<1% CPU). Order:
  1. node9900 `client.sh stop`.
  2. node9901 `EFS_CLUSTER=orcd-mini ./stop.sh`; the daemons stopped cleanly.
  3. Repoint both `src` symlinks.
  4. `start.sh` ("already formatted; preserving existing storage").
  5. node9900 `client.sh start`.
- The devops copy on node9901 was redeployed from devops `b7aac1f`; the old
  copy still passed the removed `--storage-engine` flag.
- `EFS_PACKED_WRITES=0` was dropped from node9900's `client.sh`.
- Existing data (283.58 GiB per node) remained readable from both mounts.
  Native IB RDMA came up on mlx5_0 for all daemons and both clients. The log's
  `peer-receives=4` is the credit window plus one: 6 receives give a window
  of 3.

## Environment

orcd-mini: three efsd on node9901 (10.1.99.1:17432–17434, storage
`/data1/efs/n1`–`n3` on the xiRAID volume, 800 GiB quota each, files engine,
direct I/O), RDMA mlx5_0, `.efs` commands on, expiry off. Clients: node9901
`/data1/efs/mnt` and node9900 `/data1/efs/orcd-mini-mnt`. Raw outputs stay
on node9901 under `/data1/efs/testsuite/09f8af09c2be-live/` and
`/data1/efs/logs/`.

## Results

| Suite | Command | Result |
| --- | --- | --- |
| POSIX, one mount | node9901 `EFS_CLUSTER=orcd-mini ./smoke.sh` (jobs 4, 30 s timeout) | **PASS 216/216**, 13.5 s (`posix-20261016-165512.tsv`) |
| Two-client, node9901 + node9900 | `scripts/orcd_mini_posix2.py RESULTS.tsv` from the Mac | **PASS 67/67**, 287 s (round trips through the login node) |
| Durability prepare / remount / verify | node9901 `EFS_CLUSTER=orcd-mini ./smoke-persist.sh` | **PASS 27/27** (`persist-{prepare,verify}-20261016-170115.tsv`) |
| Unit suite `make test` | test tree `/data1/efs/testsuite/09f8af09c2be`, `run_all.sh` | 766 commands, **4 environmental failures** (see below) |
| Private GC + POSIX gate, direct | `tests/live/gc_reclamation.py --mode direct --posix --root A --root B` | **FAIL** after 40 PASS lines: fsync EIO at `offline-member` after the crash-restart step |
| Fault suite, integration, direct, RDMA | `EFS_FAULT_BIN=<tree> tests/faults/run_faults.sh /tmp/efs-fault-09f8af09c2be --integrate --mode direct --transport rdma --rdma-device mlx5_0` | sim **6/6 PASS**; integration **FAIL 4/4** (fsync EIO) |

**Unit-suite failures, all environmental.** LeakSanitizer is broken on
node9901: a trivial leak program reports the same fatal error. The three
affected tests (`test_readindex_pair`, `test_raft_ack_window`,
`test_host_outbox`) pass with `ASAN_OPTIONS=detect_leaks=0`.
`check-architecture` needs the omitted `results/` and passes 5/5 on the
checkout.

**GC and fault-suite failures.** Once a daemon stops uncleanly, it logs
`recovery uncertain; COMMIT returns EIO until verified recovery or fresh
format`. Its fragment scan passes (`bad=0`), but no code path clears
`recovery_uncertain` (`src/server/store.c`). Any write whose holders include
that node fails fsync with EIO. The fault harness shares one private cluster
across its cases, so all four integration cases fail after the first crash.
The GC gate hits the same fence after its four-server crash restart.
`4d6530dd8`, the build deployed before this, fails the identical GC step with
the identical 40 PASS lines. These failures are therefore not caused by the
group A removal; they predate it.

**Test fix in this commit.** `gc_reclamation.py` still expected the 2–3 byte
PUT reply that preceded `c4178ea81`. It now checks the 11-byte reply
(status, path and 64-bit verifier).

## Reproduce

1. Build each node's release from a clean `git archive` of the commit. Use
   the private libfuse (`PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig`,
   `FUSE_LIBS` with its rpath). Pass `EFS_GIT_ID=<12-hex>` so both nodes
   share one build ID; the HELLO gate requires it.
2. Check `fuser -m` on both mounts and confirm no users.
3. Run the stop/switch/start order above.
4. Run the suites in the table. For the GC and fault gates, use private roots
   (the fault harness refuses `/data1`) and remove the fixture directories
   afterwards.
5. Remove the two-client suite's `orcd-mini-posix2-*` directory from the
   mount.

## Limitations and remaining gates

- No loaded or multi-hour workload was run. The user's tiny-file workload is
  not re-measured on this build.
- Crash-path qualification (GC crash restart, fault integration) stays
  blocked until W71 verified recovery exists. The harnesses also need
  per-case fresh clusters, or an expectation of the fence.
- The LeakSanitizer gate needs a host where LSan works.

## Redeploy of devel `f4a373b13` — 2026-10-16 22:42Z

**Deployed; smoke PASS.** orcd-mini moved from `8391950f0` to `f4a373b13dc0`
(version `v0.3.2-pre-alpha-592-gf4a373b13`). The only runtime change is the
per-thread KV read-buffer leak fix (`49167f2cf`); the other commits since
`8391950f0` change docs and tests only.

- **Builds** (clean `git archive`, shared build ID):
  - node9901 `/data1/efs/releases/f4a373b13dc0`: efsd
    `22decf7e6b2d09fcfa7b0c7e607e6a9666b3a01aa817cfb0808a9f025d7b2bd7`,
    efs-fuse `339b4696d42f5d438d22c4d6c9b2366b6bf903fb4ef4e1900cd10b7ba9302117`.
  - node9900 `/data1/efs/orcd-mini-client/releases/f4a373b13dc0`: efs-fuse
    `19e4050c59f0eda3177eb2bc53e36f02faf29b6d4287ab926766e0b0231ed38c`.
- **Procedure** (as above):
  1. `fuser -m` showed no users on either mount.
  2. node9900 client stop, then cluster `stop.sh`. All three roots logged
     `stopped clean`, so the W71 fence was not triggered.
  3. Switch both `src` symlinks, `start.sh` (storage preserved), node9900
     `client.sh start`.
- **Checks:**
  - No `recovery uncertain` lines; RDMA up on the daemons and the node9900
    client; 421.92 GiB per node readable.
  - `smoke.sh` POSIX **216/216 PASS** (14.1 s).
  - A 200-file probe on the idle node9900 mount: create / stat / open /
    unlink p50 0.83 / 0.35 / 0.49 / 0.48 ms.
  - The idle probe is not comparable with the loaded probes above.
