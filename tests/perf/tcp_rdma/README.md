# TCP / RDMA benchmarks

These programs measure whether RDMA helps. They do not assume TCP is slower, and they do not switch the live cluster. Port **19810** (and the retired 19820) is refused. Nothing here starts `efsd`, remounts a client, or exports `EFS_TRANSPORT` into a running daemon.

An echo completion is not a durable filesystem completion. `fsync` and a metadata Raft commit are different clocks. Each result line carries a `completion` field so those clocks stay apart.

Build on an fcstor, in `/tmp/efs`, after rsync. Do not run `make` in `$HOME/git/efs` (the login node is AVX-512; the fcstors are Zen2). A full matrix takes many minutes. Start it in a screen on a node (`efs-bg.sh`), not in the login-node shell.

```bash
rsync -a --delete --exclude='/mnt/' --exclude='*.log' "$HOME/git/efs/" /tmp/efs/
cd /tmp/efs && make tests/perf/tcp_rdma/xprt_bench
./tests/perf/tcp_rdma/xprt_bench self-check
```

`self-check` does not open a device. It checks the payload pattern, the percentile rank, and that a 128 KiB frame does not fit the 72 KiB RDMA pool while a 64 KiB fragment frame does.

## 1. Transport echo — `xprt_bench`

`xprt_bench` is a private echo of `EFS_MSG_BENCH_PUT` over the same TCP and RDMA SEND path as efs. It is not `efsd`. efsd answers that message with a one-byte ACK; this server echoes the payload. Pointing the client at efsd fails the byte check.

```bash
bash tests/perf/tcp_rdma/run_xprt.sh /tmp/tcp-rdma-out
```

What the runner does:

- Selects `--transport tcp` or `--transport rdma`. `auto` is not accepted.
- RDMA is strict. A failed QP upgrade is `upgrade_failed`. A send that would use the TCP side-channel is not sent: a frame larger than `max_frame` is `rejected_oversize`. A send that still lands on the other transport is `rejected_fallback`. Those rows are not RDMA results.
- Warms up inside the process, then runs one discarded `phase=warmup` pair, then **five** measured repeats. Inside each repeat, TCP runs and then RDMA runs.
- Checks the echoed bytes on every reply.
- Writes `config.json` (host, CPU model, HCA names, build id, pool size), `results.jsonl` (one object per line), and `summary.json`.

`EFS_XPRT_QUICK=1` is a wiring check (short counts, one repeat). Do not quote it.

The default host is `127.0.0.1`. After upgrade that is a same-host QP (`dev_probe_first`), labeled `placement: loopback` in `config.json`. Quote a two-host run for a cross-host number. On the server machine:

```bash
EFS_RDMA_BUFS=64 ./xprt_bench server --bind 0.0.0.0 --port 19960 \
    --expect 1 --ready /tmp/xprt.ready --stats /tmp/xprt-server.json --series 1
```

On the client machine, after `ready` exists, run `run_xprt.sh` with `EFS_XPRT_HOST=<server-ib-ip>` and `EFS_XPRT_SERVER_ALREADY=1`. Copy the server JSON line into `results.jsonl` before `summarize.py`. Use the same binary, the same `EFS_RDMA_BUFS`, and no CPU pinning unless both transports are pinned the same way.

### What is measured

| Mode | Shape | Reported |
|---|---|---|
| `latency` | one request outstanding, payloads 64, 251, 256, 1024, 4096, 16384 | median, p99, p99.9 (µs) |
| `throughput` | 65536 and 131072 bytes, depth 1, 4, 16, 64, then 4 clients at 65536 / depth 1 | GiB/s, client and server CPU-seconds per GiB |
| `idle` | one request after a gap of 0, 1, 10, 100, 1000 ms | median, p99, p99.9 of the exchange after the gap |

251 is the largest payload whose frame (5-byte header + payload) is still an inline SEND (`EFS_RDMA_INLINE_MAX` is 256). A 256-byte payload is not inline. The JSON field `inline` records that.

A 128 KiB payload is the filesystem chunk size. Its frame does not fit the RDMA pool (`EFS_RDMA_BUFSZ` is 72 KiB), so efs would put it on the TCP side-channel. The bench rejects it with `rejected_oversize` and does not time that fallback. A 64 KiB payload is the fragment size and does fit. That is the RDMA chunk-shaped row.

`EFS_RDMA_BUFS=64` is set in the bench processes so one QP can hold 64 posted receives. It is not exported into `efsd`. Depth above 64 is `rejected_qp_depth`.

For `throughput`, `median_us` / `p99_us` / `p999_us` are **batch wall time / depth**, not a one-outstanding RPC latency. The rate is `gib_s`. Server CPU includes the single warmup batch. Idle `elapsed_s` is the sum of the exchanges and does not include the gap.

Percentiles use nearest rank: index `ceil(p/100 * n) - 1`. `summary.json` then takes the median of those values across the five measured repeats.

A recv that does not finish within 5 s counts as `timeouts`, and the series stops. `errors`, `timeouts`, and `fallbacks` are on every line.

## 2. SEND hardware baseline — `run_send_baseline.sh`

efs uses RC SEND/RECV, not one-sided RDMA WRITE. The closest device baseline is perftest:

```bash
bash tests/perf/tcp_rdma/run_send_baseline.sh /tmp/send-baseline-out
```

This runs `ib_send_lat` (64 B–16 KiB) and `ib_send_bw` (64 KiB and 128 KiB, tx-depth 1/4/16/64), five times. `completion` is `ibv_send`. It is not an efs RPC and not an fsync. If `ib_send_lat` is missing, the script writes `status: tool_missing` and exits 2. It does not invent a number.

Set `EFS_IB_DEV` when the host has more than one HCA. Loopback is labeled as such. For two hosts, start the server on one node and set `EFS_BASELINE_HOST` and `EFS_BASELINE_SERVER_ALREADY=1` on the other — perftest serves a single client and then exits, so the packaged script starts both sides on one machine.

## 3. Filesystem — `run_fs.sh`

Separate from the echo. The operator brings up a **private** export with `EFS_TRANSPORT` set to `tcp` or `rdma` (not `auto`) and passes that mount in. The script checks all of the following and exits if any fail:

- `efs-fuse` command line is not `:19810` or `:19820`
- `/proc/<pid>/environ` has `EFS_TRANSPORT` equal to the requested value
- the fuse log agrees (`RDMA transport up` present for RDMA, absent for TCP)
- `findmnt` reports `fuse.efs-fuse` and `stat` of the mount succeeds

```bash
EFS_BENCH_TRANSPORT=rdma EFS_BENCH_MOUNT=/mnt/private \
    bash tests/perf/tcp_rdma/run_fs.sh verify /tmp/fs-out
```

| Subcommand | What it measures | Completion |
|---|---|---|
| `stream` | 256 MiB non-zero `dd bs=1M conv=fsync` (not `time_based`) | `fsync` |
| `meta` | create / stat / rename latency | `filesystem_metadata` (includes the Raft commit) |
| `cold-write` | 4 MiB non-zero file, `fsync`, sha256 of the source | `fsync` |
| `cold-read` | one read of that file on a **different** host, sha256 checked | `filesystem_read` |
| `idle` | `stat` after 0/1/10/100/1000 ms gaps | `filesystem_stat` |
| `overlap` | stream window vs metadata window, same transport | |

`cold-read` refuses to run on `EFS_BENCH_WRITER_HOST`. A read on the writer is served from the client dcache and is not a server read. Run `stream` on one client and `meta` on another during the same interval, then `overlap` with `EFS_BENCH_STREAM_JSON` and `EFS_BENCH_META_JSON`. Alternate the whole filesystem set the same way as the echo: TCP, then RDMA, five times, on the same hosts and the same build.

## Reading a result

`summarize.py` keeps warmup rows out of the medians. A group is `ok` only when every measured repeat is `ok`. `rejected_oversize` is a real result (the frame does not fit), not a crash. Anything else, including a server that counted TCP requests during an RDMA series, is a failure. Exit status is non-zero.

Do not compare an `ibv_send` row, a `transport_echo` row, and an `fsync` row as if they were one number.
