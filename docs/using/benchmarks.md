# Developer benchmarks

`efs-bench` is a developer measurement tool, outside the alpha user surface.
Local `--bench data`, `--bench meta`, `--bench io`, `--bench io-blake3`,
`--bench blake3` and `--bench principles` retain their own help and scratch rules.

## Published cluster data

```sh
efs-bench NODE:PORT --size 1G --io-size 4M --workers 8 --id 170
efs-bench NODE:PORT --read --size 1G --io-size 4M --workers 8 --id 170
```

One seed discovers the committed membership. Both commands initialize a normal
client session and use the normal ticketed client API. A write creates
`/bench-data-170-w0/payload` through `/bench-data-170-w7/payload`, writes sequentially and checks
synchronous per-file REPORT, final flush/REPORT and session shutdown. An existing
fixture is an error; choose a fresh ID rather than overwriting it. Without `--id`,
a write uses its PID. Data is deliberately retained for independent readback;
remove only those named worker directories through a mount when finished.

`--size` is aggregate logical bytes, split across workers with any remainder
assigned to the first workers; each file must be nonempty. `--io-size` is the
request size (default 1 MiB, maximum 16 MiB), with an exact final short request.
`--workers` is 1–64 (default one). The payload is `A` bytes, matching the write
cost workload; this is a byte/length check, not a version-identity oracle.

A read requires the original ID and worker count. Optional `--size` also checks
each file's expected EOF. Without `--time`, it reads every file once. With
`--time`, it repeatedly checks blocks until the duration expires. A timed write
starts its common I/O clock after all workers finish namespace/buffer setup,
stops at the first of its byte limit or duration, publishes the completed bytes
and retains its actual-length files. Timed read results describe only bytes
actually checked; they never qualify a whole fixture implicitly. All errors
return nonzero and cannot print `BENCH_OK`. Wall time includes client setup,
publication and shutdown, not just data RPCs.

`--meta --size N` means N bytes **per file**, with normal client initialization
and checked publication; it is separate from the sequential-file data workload.
Its existing `--files`, `--dirs`, `--phases`, `--workers` and `--keep` options remain.

`--store` and the raw-fragment `--window`/synthetic GET modes are removed.
`--time` alone remains the explicitly separate ACK-and-discard network ceiling;
it does not write files or measure durable storage. `scripts/bench_profile.py`
uses bounded published fixtures for its remote store/read rows. Run
`python3 tests/test_bench_published.py` after `make test-bins` for the three-node
cold-FUSE regression; it is also in `make test-long`.
