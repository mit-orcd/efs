# Intel cluster and xorinox client CPU profile — 2026-10-23

**Analysis only; no fix or A/B in this report.** User request: profile the
Intel cluster and its xorinox client, find the hot paths and report where the
raw perf data is stored. All three workloads are **not CPU-bound**: the four
efsd use 1.1–1.7 cores of 8 per host, and the client 0.4–0.7 of 16.
On-CPU samples therefore show where cycles go, not what limits throughput.
The clearest limiter is in the tiny-file phase: the client's 16-position
identity-admission window turns opens and closes into 1 ms BUSY retry
loops.

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
- **Script:** the capture script is reproduced below; its text reports were
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
- **Hashing:** blake3 checksum verification is 5.6% and 10.9% self in the
  two daemons.
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
- **Admission retry sleeps, 21% of on-CPU samples:** these are
  `clock_nanosleep` entries from 1 ms `usleep` loops. They come from the
  release path (`efs_fuse_release_ino`, close admission spin, 790 samples),
  from create (`efs_open_acquire_created` → `efs_open_acquire_fileid_mode`,
  559) and from open (100).
- **What the loops wait for:** both wait on the identity admission. The code
  says it is "a 16-position window shared by every thread", and it answers
  BUSY while full. With 32 writers the window is full most of the time.
- **Taking positions:** `opid_take_locked` is 6.8% self, through
  `efs_client_lease_begin` from `efs_open_release_pending` and
  `efs_open_retry_worker`.
- **Conclusion:** this is the most likely cause of 123 files/s. It is not
  measured as wall time; an off-CPU capture is needed to quantify it.

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
