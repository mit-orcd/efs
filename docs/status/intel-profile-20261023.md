# Intel cluster and xorinox client CPU profile — 2026-10-23

**Analysis only; no fix or A/B in this report.** User request: profile the
Intel cluster and its xorinox client, find the hot paths and report where the
raw perf data is stored. **Corrected after an independent review (see the
end of this page).** Samples divided by the sampling rate suggest the four
efsd use well under their cores and the client less, but that is a rough
estimate: it cannot exclude serialization or short saturation. In the
tiny-file phase, the client's identity admission keeps every new mutation
within 16 sequence numbers of the oldest unresolved one. That makes opens,
creates and closes wait behind the slowest operation (head-of-line blocking);
whether it causes the files/s plateau is still a hypothesis.

## Build, environment and method

- **Build:** devel `163241584` (merged as `7e6daa314`), the Intel fresh deploy
  of [intel-devel-testing-posix-20261023.md](intel-devel-testing-posix-20261023.md).
- **Servers:** four efsd on intel1 (10.0.0.207, n1/n2) and intel2 (10.0.0.209,
  n3/n4); TCP over RTL8126 at 5 Gb/s; direct I/O; 400 GiB quota per node;
  `.efs` on.
- **Client:** efs-fuse on xorinox, `/data1/efs/intel-mnt`, release
  `/data1/efs/intel-client/releases/163241584306`.
- **Workloads** (all from the xorinox mount, run one after the other):
  - write: 8 × `dd if=<2 GiB random fixture> bs=1M conv=fsync`;
  - read: 8 × `dd` of those files to `/dev/null`;
  - tiny: `tiny_files.py --max-files 2000000 --workers 32 --max-depth 2`,
    stopped with SIGKILL after 60 s.
- **Capture:** 8 s after each phase started, 25 s of
  `perf record -F 499 -g -p <pids>`. On intel1 and intel2 this covered both
  efsd of the host; on xorinox, the intel-mnt efs-fuse only.
- **Reports:** the per-host capture command is under "Raw data"; its text reports were
  `perf report --no-children`, `--children` and caller graphs.

| Phase | Wall time | Throughput | CPU used (samples / 499 Hz / 25 s) |
| --- | --- | --- | --- |
| write | 105.4 s | 16 GiB → 155 MiB/s | intel1 1.73, intel2 1.32, client 0.69 cores |
| read | 91.3 s | 16 GiB → 179 MiB/s | intel1 1.59, intel2 1.12 cores; client capture failed (see limitations) |
| tiny | 60.0 s | 7,374 files → 123 files/s | intel1 1.06, intel2 1.28, client 0.40 cores |

## Hot paths

**Write, servers** (intel1, inclusive):
- **Fragment store, 21.7%:** `server_write_fragment_with_sum_sync` →
  `server_write_fragment_to_path`. Of that, about 13% is `open()`, a path
  lookup and XFS create for every fragment file, and 16% is the aligned
  direct-I/O write in XFS.
- **Network:** receive 13.5% and send 13.3%.
- **Hashing:** blake3 checksum verification is 5.6% and 10.9% self. These
  are the efficiency-core and performance-core PMUs of the hybrid Intel CPU
  (`cpu_atom`/`cpu_core`), which perf reports as two tables. They are not
  the two daemons.
- **LSM lookups:** `kv_mtab_pos` and `kv_seg_probe` show up at about 1.9%
  together.

**Write, client:**
- **Fragment upload, 47%:** `put_pool_thread` → `dcache_put_now_budgeted`.
- **Hashing, 16%:** blake3 in `hash_write_fragments`, of which avx512 is 15%
  self.
- **Copies and parity:** `memmove` 9.6%, and `xor_into` (parity) 3.1%.
- **Network:** TCP send 16%.
- **FUSE:** the request loop, `fuse_do_work` → `ll_write_buf`, is about 16%.

**Read, servers** (intel1, inclusive):
- **Connection handling, 45%:** `server_handle_conn`.
- **Read and verify, 24%:** `nvme_get` → `read_fragment_verified`, of which
  12% is `efs_frag_verify`, blake3 verification of every fragment read.
- **Network:** TCP send 17%.

**Tiny files, client:**
- **CPU around admission retry sleeps, 21% of on-CPU samples:** these are
  `clock_nanosleep` entries from 1 ms `usleep` loops. They measure CPU
  spent entering and leaving sleeps, not how long threads wait or how many
  retries happen. They come from the
  release path (`efs_fuse_release_ino`, close admission spin, 790 samples),
  from create (`efs_open_acquire_created` → `efs_open_acquire_fileid_mode`,
  559) and from open (100).
- **What the loops wait for:** both wait on the identity admission. The
  binding limit is the sequence window, not the slots
  (`src/client/inode_rpc.c`, `opid_take_locked`). The client has 512
  in-flight slots, but refuses a new identity once it would be 16
  (`EFS_OPID_REPLY_CACHE`) past the oldest unresolved one. So one slow
  create, close or lease edge holds up every thread on the mount. (Earlier
  wording, "32 workers fill 16 positions", was wrong.)
- **Taking positions:** `opid_take_locked` is 6.8% self, through
  `efs_client_lease_begin` from `efs_open_release_pending` and
  `efs_open_retry_worker`.
- **Conclusion:** a hypothesis for the plateau (93, 128 and about 123–145
  files/s at 8, 16 and 32 workers), still unmeasured. It needs refusal
  counts by cause and the age of the oldest unresolved identity, from runs
  without perf attached.

**Tiny files, servers** (intel2, inclusive):
- **RPC and Raft traffic:** `server_handle_conn` 27%, `efs_conn_send_msg` 20%,
  `host_pump` 15%, `drain_inbox` 13% and `efs_raft_recv_batch` 12%.
- **Kernel network:** the r8169 NIC driver and softirq are 12–19%.
- **Overall:** at about 1.2 cores per host the servers are mostly waiting.

## Next steps (proposals, not done)

1. **Off-CPU capture**, `perf record -e sched:sched_switch -g` or offcpu mode
   in the devops `efs_dd_profile.py`. On-CPU profiles cannot show the
   latency that limits all three phases.
2. **Tiny files:** measure BUSY admissions per create and close. Test whether
   the 16-position identity window, rather than the servers, caps
   files/s, for example by comparing 8, 16 and 32 workers.
3. **Write:** the per-fragment `open()` cost (about 13% of server CPU) and the
   double blake3 work (hashed on the client, verified on the server) are
   CPU costs, not the current throughput limit.

## Raw data (outside Git)

Each `perf.data` stays on its host and needs that host's binaries
(`/data1/efs/src` on Intel; the release directory above on xorinox) for
symbols.

| Phase | Host and path | sha256 | Size |
| --- | --- | --- | --- |
| write | intel1 `/data1/efs/prof/intel-20261023/write/perf.data` | `a70f3b0390e373151a00a6af068a472e31d90369638c6ac997396dc761dfb66c` | 4.2 MB |
| write | intel2 `/data1/efs/prof/intel-20261023/write/perf.data` | `49aafbd3c3ebce92c57a55722debbc57fe7b836267981812741fcb8f32eae1f9` | 3.3 MB |
| write | xorinox `/data1/efs/intel-client/prof/intel-20261023/write/perf.data` | `2f4af927a5ef864d4ab80e25029b3a08f499dd6aa2fe7c416a562cffbb84a764` | 1.7 MB |
| read | intel1 `/data1/efs/prof/intel-20261023/read/perf.data` | `5b8af1875ded104fd51231b4e329d83a640b4a0a74cbfa81e1db10f04894d1b9` | 3.7 MB |
| read | intel2 `/data1/efs/prof/intel-20261023/read/perf.data` | `4471fac5f388dd5d09fa979e34577d81a73a0bdcc2ed9d56ca29b022c85de297` | 2.7 MB |
| read | xorinox `/data1/efs/intel-client/prof/intel-20261023/read/perf.data` | `7893718983c605429a45cc5d5b3cd8775016068b6b4b3da8d53dcb5e7c014169` | 132 KB (no usable samples) |
| tiny | intel1 `/data1/efs/prof/intel-20261023/tiny/perf.data` | `03896d063df1b3d3c7ce9d416199821e45cb3d7614b6632ec63b0d8832509136` | 2.7 MB |
| tiny | intel2 `/data1/efs/prof/intel-20261023/tiny/perf.data` | `4dda307701b8608e84ecf010735d67db2f5507548d3bf24bf8db77a2ae267735` | 3.2 MB |
| tiny | xorinox `/data1/efs/intel-client/prof/intel-20261023/tiny/perf.data` | `e40dc1e289c1a26a581f3e3392dab6c12f6b7729b4781b4f17dfcc2302d12a9f` | 1.1 MB |

Next to each `perf.data` are `self.txt`, `children.txt`, `callers.txt` and
`record.log`. The 2 GiB random write fixture is
`/data1/efs/intel-client/prof-fixture-2g.bin` on xorinox. The workload
output stays on the cluster: `/data1/efs/intel-mnt/prof/w1..w8`, and
`prof-tiny/` with 7,374 files.

Reproduce one capture with:

```bash
ssh intel1_efs 'perf record -F 499 -g -p $(pgrep -u efs -x efsd | paste -sd,) -o OUT/perf.data -- sleep 25'
```

Run it during the workload above; use the same command on intel2, and on
xorinox with `-p` set to the intel-mnt efs-fuse PID.

## Limitations

- **Read client capture:** `perf record -p` on the xorinox client logged open
  failures for exiting threads and wrote no samples, so read-phase client
  hot paths are missing.
- **One run per phase:** there is no repeat and no baseline build.
- **Lock waits:** the percentages are on-CPU only; lock waits, disk waits
  and RPC latency are not measured.
- **Other load on xorinox:** another session's gate clusters ran there at the
  same time. The capture was limited to the intel-mnt client PID, but CPU
  contention from that load is not excluded.

## Corrections from the independent review (October 23)

An independent review decoded the eight usable recordings and verified all
nine hashes; the raw captures are unchanged. Its extracts are in
`/tmp/efs-independent-perf-20261023/` on the user's Mac. Corrections,
accepted here:
1. **Two tables per Intel host:** these are the `cpu_atom` and `cpu_core`
   PMUs, confirmed in the capture header. They are not the two daemons.
2. **"Not CPU-bound" was too strong:** samples divided by the sampling rate
   are only a rough CPU estimate.
3. **Sleep stacks:** these are CPU spent around sleeps, not wait time or
   retry counts.

Refined figures:
- **Write, servers:** fragment store 21–29%, `openat` 9–16%.
- **Read:** fragment read and verify is 24–27% on intel1 and 40–45% on intel2;
  verification alone is 23–30% on intel2.
- **Tiny files:** the background `efs_open_retry_worker` is about 8% of
  client samples.

Head-of-line blocking at the sequence window (above) is the leading
hypothesis for the tiny-file plateau. Do not widen `EFS_OPID_REPLY_CACHE`
without data: it bounds per-client server receipt retention, and it would
only move the wall to the next slow operation. The diagnostic follow-up is on
branch `devel-performance` (`EFS_OPID_STATS`).

