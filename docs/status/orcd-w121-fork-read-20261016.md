# W121: ORCD FUSE direct-I/O reads racing fork

**Verdict: OPEN platform correctness gate; kernel remediation deferred by the
user on October 16, 2026.** The benchmark trigger can be avoided, but ordinary
applications that fork while another thread reads remain affected. Do not
interpret a fork-isolated benchmark PASS as a W121 fix.

## Evidence

Baseline source `fa29f9277eb604f7212bad164b283df85697c9a0`, four storage
servers fcstor003–006 and seven FUSE clients fcstor007–013, native RDMA
mlx5_2, files engine/direct I/O, `.efs` enabled; packing, containers, expiry
and repair experiment disabled. Fresh experiment stores had a 17 GiB quota
per server. Client OS: RHEL 9.8, kernel `5.14.0-687.39.1.el9_8.x86_64`,
Python 3.9.25, private libfuse 3.16.2. All EFS processes ran as erbmi1.

- Quiet single-client/eight-writer 60-second run: 9,581 cycles, no mismatches.
- Original seven-client load, eight writers and two query-launching threads
  per client, 90 seconds: 30,615 cycles, 84 wrong first reads. Every immediate
  retry and all retained-file reads from fcstor007 matched the expected bytes.
- Temporary client-only tracing on fcstor007, otherwise unchanged source:
  13 mismatches in 5,422 cycles. Every affected request had the **correct**
  payload at both `efs_client_read` completion and `fuse_reply_buf` entry.
  The application nevertheless observed its previous payload.
- Minimal fixed-file test, eight reader threads, 30 seconds, each destination
  prefilled with `?`: concurrent `fork()` produced **86,629 failures out of
  433,368 reads**. Successful 512-byte reads left the `?` bytes in the caller.
  Replacing fork with `posix_spawn()` produced **0/486,164 failures**.
- Seven-client original load with absolute interpreter path and
  `subprocess.run(close_fds=False)` selecting Python's posix_spawn path:
  **0/33,527 readback failures**, all queries passed. This is corroboration,
  not the permanent harness strategy or a kernel qualification.

The installed `fuse.ko` imports `iov_iter_get_pages2`, rather than the pinned
page extraction API. The observed failure matches upstream
[`738adade96b2`, fuse: Fix missing FOLL_PIN for direct-io](https://android.googlesource.com/kernel/common/+/738adade96b2ec414a44f3b1ed891fec3e0c03dd).
An in-flight read can complete into a page from which fork/COW has separated
the parent's buffer. This explains correct daemon bytes, successful read
length, stale application bytes and correct subsequent reads. This diagnosis
is supported by controlled triggering, not a successful patched-kernel A/B;
the latter remains required. No Raft/KV/cache correctness change is justified
by this evidence.

## Reproduction and remediation

Run the committed regression on a new directory in an existing EFS mount:

```sh
python3 tests/live/orcd_raft_kv/fork_repro.py /tmp/efs-mount/w121-fork-new fork --seconds 30
python3 tests/live/orcd_raft_kv/fork_repro.py /tmp/efs-mount/w121-spawn-new spawn --seconds 30
```

Each creates eight 512-byte fixtures. Exit 1 means wrong reads or incomplete
workers. Exact bytes and successful read lengths are checked. Fixtures remain
for independent reads; remove only these named test directories afterwards.
Repeat both modes on a kernel containing the upstream fix/equivalent backport,
then repeat the original mixed load and peer/clean-restart checks. The ordinary
`open()` calls do not request O_DIRECT; EFS sets FOPEN_DIRECT_IO itself. Server
`--direct-io` is a distinct setting and changing it does not repair this client
kernel path.

The reproducible harness fix in `tests/live/orcd_qvote/workload.py` starts
query processes **before** mounted-I/O threads. Each query process can launch
helpers without forking the address space that owns read buffers. It retains
the same eight writers and two concurrent query loops per client. This permits
Raft/KV measurements with the requested profile while W121 stays OPEN.

No kernel replacement is available now; the user explicitly deferred installing
one. No kernel changes, global FUSE cache-mode changes or data-path retries
were made to hide mismatches.

Raw evidence is outside Git:
`/home/erbmi1/orcd/scratch/efs/orcd-test/w121-20261016/`:
`*-before.json`, `fcstor007-diag.json`, `fuse-diag.log`, `fork.json`,
`posix-spawn.json`, `*-spawn2.json`. The `*-spawn.json` exploratory run is
excluded: its script upload raced process startup and it overlapped another
diagnostic workload. Diagnostic source copies are retained there. Earlier
binary identities and original four-round results are in the
[queued-read-vote evidence](orcd-queued-read-vote-20261016.md).
