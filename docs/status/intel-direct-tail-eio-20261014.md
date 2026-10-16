# Intel buffered-tail EIO correction — 2026-10-14

**PASS: reproduced reader defect fixed, main Intel and all four clients
redeployed; bounded smoke only.** Follow-up to the
[live performance investigation](intel-live-performance-20261014.md), which
found old-tree EIO while observing xorinox's MD5 crawl. No W100 reopening,
performance change, full-suite or full-inventory acceptance claim.

## Cause and fix

The first three reproducible failures are inodes1885194,2158298,2162394 in
`tiny-intel2-1m-20261010`. Authoritative metadata names one committed delta
per inode, each with three nodes and an exact object generation. All nine
corresponding physical fragment files exist and have length65,568bytes:
65,536bytes of payload plus a32byte digest. Good controls have69,632byte
fragments: the same payload plus a4,096byte padded digest page.

`read_file_bytes` in [store.c](../../src/server/store.c) opened these files
with O_DIRECT and requested a4,096byte tail at offset65,536. Linux returns
the short final32bytes at EOF for the buffered stored format. The code
required `rn == 4096`, so it marked the complete digest unavailable. The GET
handler refused those fragments; fewer than two usable fragments resulted in
client `EFS_ERR_DECODE`/native EIO. This is stored-format interoperability,
not proof of missing bytes, bad digests or TCP loss.

Fix `f76101e8fc5469e5999e25cc1c0db6418416f9f3` accepts a tail read only
when at least the complete32byte digest is available. The aligned direct-I/O
request remains unchanged. The GET handler still hashes the payload and
compares the recorded digest; missing/truncated digests and corruption remain
unavailable. No fragment rewrite, padding migration, checksum bypass or
buffered fallback was used. All nine original fragment file size/SHA256 pairs
match before and after redeployment.

## Deployment

Frozen source: `f76101e8fc5469e5999e25cc1c0db6418416f9f3`, branch devel,
build ID `f76101e8fc54`, version `v0.3.2-pre-alpha-453-gf76101e8f`, common
build metadata time `2026-10-14T19:51:00Z`. Native GCC Makefile build, optimized
with debug/frame pointers and native ISA. Source-only reproduction archive
retained with the run. Builds completed before stopping production services.

Environment/topology remain those in the linked performance and
[deployment report](intel-external-clients-deployment-20261014.md): two Core
Ultra7 256V Intel hosts, four daemons,400GiB/node, TCP/direct/files. `.efs`
client/server and expiry=0; Intel links5Gb/s,
external AMD/xorinox links2.5Gb/s. Final `/proc` checks confirm all eight main
processes use this release and the exact optional-off environment; all four
daemons have `--direct-io`. Persisted commands policy is version1/mask0.

|Host|efsd SHA256|Intel-client efs-fuse SHA256|
|---|---|---|
|Intel1|`84e11c5345b5cdb8ff17331018c348f93522f7e5bad07a7871fd4c37ccb13f60`|`695e880c7a975bcb092b1847c25e6c3ce5cb63c51c0d70394f50c483e35dc450`|
|Intel2|`39a669eb6c16de24398a48a9c14d86b21a3a02488a7611a257fff9819a48dd2c`|`a565415d77bdbfb47d1b05a535ec35e4076ea1d56d749ed24aa082beda16e2cf`|
|AMD|Not installed in independent client tree|`ed76029b2915d91ba8ebbfd760db84ce42d21ad3a504eb98117934efcc8fbf86`|
|xorinox|Not installed in independent client tree|`e3a21bf94b53ec2b5f814c4b50425f793219040a37da538dc8fb7bc149c76e4a`|

Intel source link is `/data1/efs/src` → `/data1/efs/releases/f76101e8fc54`.
External source links remain independent:
`/data1/efs/intel-client/src` → its own `releases/f76101e8fc54`.
Intel mounts `/data1/efs/mnt`; AMD/xorinox `/data1/efs/intel-mnt`.
Old releases retained; no firewall or AppArmor change required.

The known AMD writer and xorinox crawler received graceful TERM before
unmounting. Both completed their shutdown handlers. The interrupted crawler
manifest finalized with105,057 successful hashes/1,113 errors; it was not a
complete inventory. All four production clients drained while quorum was
alive, then the four owned main daemons stopped. New peers launched before
waiting for recovery, followed by all four mounts. Existing stores preserved;
no formatting occurred. Intel1's formatting marker exists; Intel2's installed
startup source sets LOCAL_HOST=intel2_efs and HOST=intel1_efs, excluding its
Intel1-only formatting block. An initial extra marker precheck prevented
Intel2 mounting; source/host checks established the safe mounts phase before
completing it. No product read failure resulted from that setup correction.

The six private Intel processes and six AMD-local processes retain exactly
their previous PIDs, arguments, binaries and optional-feature profiles. No
VM deployment was changed. Long writer/crawler jobs were left stopped after
the upgrade; only the requested smoke ran. The writer's completed
files and crawl manifest remain in place for a human-selected resume.

## Smoke evidence and independent reproduction

Only smoke tests were authorized/run, not POSIX/concurrent/sanitizer suites.

1. **Old-code control reproduces FAIL.** Run the committed
   [fragment_tail_smoke.py](../../tests/live/fragment_tail_smoke.py) with the
   previous `85f2a199aa39` binary directory: padded-tail GET passes, truncating
   only padding to a valid32byte tail causes GET unavailable. The fixture is
   its own temporary16MiB-quota TCP/direct/files daemon, not production.
2. **Corrected focused smoke PASS on both Intel hosts.** Seven assertions per
   host: initial padded tail;32byte and4,096byte stored tails accepted with
   exact payload/digest;31byte and absent tail refused; payload corruption and
   digest corruption refused. Temporary daemon terminated and fixture removed.
3. **Mounted read smoke64/64 PASS from each of four clients.** The first32
   errors and first32 successes from the saved crawler manifest form the
   bounded sample. All previously unreadable files now return exact stat size
   and EOF; all successful controls retain their recorded MD5. Every sample's
   length/SHA256 matches across all four mounts. `.efs` absent on all four.
   This proves32 recovered files per client, not all1,113 historical errors.
4. **Four-host write/read smoke PASS.** Each host exclusively creates one
   file beneath owned `eio-tail-smoke-20261014-f76101e8fc54`, containing
   `bytes(range(256))*((role+1)*256) + bytes([role])*17`, roles0–3.
   Each write flushes and fsyncs. All four clients read all four files and
   check exact bytes and EOF (65,553/131,089/196,625/262,161bytes). Only those
   four smoke files and their directory are removed afterward.
5. **Final health PASS.** Four nodes up/zero down,4/4 identical versions,
   mounts available, process/profile and preserved fragment checks pass.

Prerequisites for the focused reproduction: Linux filesystem supporting
O_DIRECT, an unprivileged service account, native built `efsd`, Python3, and a
free loopback ephemeral TCP port. No root packages or policy change needed.

```sh
python3 tests/live/fragment_tail_smoke.py /path/to/old-binaries
python3 tests/live/fragment_tail_smoke.py /path/to/fixed-binaries
```

The old command is expected to fail at `stored tail length 32`. The new command
must pass all seven checks in seconds. The harness creates its own checksum-
validated64KiB PUT and modifies only its disposable image; it never edits a
deployed fragment. It bounds socket operations at5seconds, startup at10seconds
and shutdown at15seconds. This reproduction requires no private capture or
pre-existing user data. Native daemon build command:

```sh
make -j4 EFS_GIT_ID=f76101e8fc54 \
  EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-453-gf76101e8f EFS_GIT_BRANCH=devel \
  EFS_BUILD_TIME=2026-10-14T19:51:00Z efsd efs-fuse efs-mgmt
```

For deployed smoke reproduction, verify the exact FUSE mount with `findmnt`
before accessing any fixture; do not write to an unmounted backing directory.
The three known failed paths and three control paths are listed in the linked
performance report; read at most1,025bytes/file, compare stat size and exact
EOF, then compare hashes across hosts. This is a bounded representative smoke,
not an instruction to restart the whole crawler.

Raw evidence is outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-eio-tail-fix-20261014/`.
It includes the old failing control, both fixed smoke outputs,64file/client
samples, cross-host results, original fragment hashes, before/after process
inventories and final status. `artifact-sha256.json` records useful hashes.
Source-only archive SHA256:
`56b6254956f234c6fd7cc45c21d9db6978f6927bcde674a0fa13b777ca6cfba3`.
Both fixed focused-smoke output files have SHA256
`b5470276b4ad14d6d3aec0e23ab2715522bf51d180d4f6ede07bcb1f36f4029b`;
mounted read-summary SHA256
`04d4960a6bc330e4d917a13689ab06b20fdd7fdd175f01f1be2b9949462f4d13`.
Historical result copies included in the first staging archive were removed
only from the new release directories before activation; existing production
payloads and older release directories were not affected.
