# W91/W92 bounded command results and streaming export

The user requested a 100-result default, an override up to 10,000, a truthful
notice about additional results, and `--file PATH` for exporting all results.
This applies to `.efs/ls` and `.efs/find`; `.efs/stats` remains one aggregate.

## User behavior

```sh
/data1/efs/mnt/.efs/find .txt                 # at most 100 displayed entries
/data1/efs/mnt/.efs/find .txt --limit 10000   # --size is an alias for --limit
/data1/efs/mnt/.efs/ls --size 500 -l
/data1/efs/mnt/.efs/find .txt --file /home/efs/all-text-paths.txt
/data1/efs/mnt/.efs/find .txt -0 --file /home/efs/all-text-paths.bin
/data1/efs/mnt/.efs/find .txt --json --file /home/efs/all-text-paths.jsonl
```

Display sorting applies to the selected subset, not the global top N. Notices
always go to stderr, keeping NUL output exclusively paths. Capped output says
“more results exist” when an additional authorized matching entry was seen,
or “more results may exist” if remaining candidates were not examined. No
unmeasured total or estimate is invented. JSON includes `truncated` and page
continuation/completeness details. `complete: true` means the delivered page
was validated; it does not override `truncated`.

`--file` requires a destination argument and validates its parent by opening a
temporary file before querying. Empty paths, stdout `-`, directories, symlinks
and non-writable existing files are refused. Export streams in index order,
in batches of up to 1,000 entries, independently of the display limit. Global
sorting/reverse/directories-first options are rejected for file exports.
Text escapes unsafe filename bytes; use `-0` for lossless native paths.
JSON export is NDJSON: snapshot header, one `entry` object per line, then a
completion/count record. The scratch file itself is excluded if inside scope.

The destination is atomically replaced only after all pages validate and its
scratch file is flushed and fsynced. Errors remove the scratch file and leave
any existing destination intact. This does not claim power-loss durability
of the destination directory entry. A successful export knows the number
actually written; ordinary capped queries do not count every match.

## Server and resource contract

Wire version 4 extends v3 with a session token, physical index cursor, page
limit and request nonce. The compact `v3.BASE64URL` synthetic filename fits
within 255 bytes even with a 120-byte raw filename pattern and all filters.
Legacy wire versions and command paths retain their existing behavior/limits.
Server and FUSE client must be upgraded together for new command scripts.

One export/catalog lease per full-metadata host retains an immutable published
generation. Continuations route to that host. This allows normal publication
while exporting and avoids rescanning each page from the start. Reference
counts for catalogs, shared partitions and namespace contributions are atomic.
The retained catalog is independently bounded by the existing configured
catalog cap; published/candidate plus one lease can retain up to three catalog
versions per host (some allocations are shared). Replies remain under the
shared 64 MiB budget and eight-request admission limit.

Each page scans at most 100,000 index entries and emits at most 10,000 results
within a 16 MiB response. Scan exhaustion returns a cursor, not the old
million-entry EBUSY retry loop. Empty pages can advance over outside-scope or
nonmatching candidates. Both ls and find use the immutable name index in this
protocol. Fresh quorum/ReadIndex, permissions, ancestry and name bindings are
checked for each page; this is not a live filesystem walk. Mutations that
invalidate a delivered binding or permissions refuse that page. Permission
revocation, restart, lease expiry or resource refusal can fail the whole file
export; generations are never silently mixed.

Leases expire after 60 seconds idle or one hour total, are explicitly closed
by the script, and are swept even without further RPCs. FUSE lookup/open
replays are idempotent through a per-command nonce. Unrelated commands cannot
release another session. A second concurrent export on a busy publisher may
be refused or routed to the other capable publisher. No unbounded snapshot
history is retained.

## Validation

Native catalog tests compare incremental builds against independent full
builds and now verify cursor progress, page boundaries without loss/duplication,
and retained-catalog lifetime. Python tests cover defaults, range errors,
C/Python codec round trips at maximum pattern length, unsafe filenames,
multiple pages, clipping across short pages, NDJSON export, destination
validation, and preserving an existing file after a later page fails.

Private direct/TCP tests cover two mounted clients, 215-entry default/limit
oracles, NUL and NDJSON exports, retained-generation identity through a fresh
publication, no duplicates/missing entries, resource admission, close/stale
sessions, and chmod revocation. Full POSIX/peer and restart gates accompany
these tests. Deployment and million-file evidence will be recorded below.


## NUC acceptance and deployment

Installed source/binaries: **`abcd9b7b1b6f`, devel-find-stats**, all four nodes
and both clients, direct I/O/TCP, `.efs` enabled, existing stores preserved.
The generator was already stopped; no loader was restarted for these read
checks. Previous binaries/source are retained at
`/data1/efs/src-pre-pages-abcd9b7b-20261009T035940Z`.

Both `/data1/efs/mnt` and `/data1/efs/mnt2` return 100 root `.txt` matches by
default and 10,000 with the override, with `truncated: true` and the additional
results notice. On the second mount these took 42.9 ms and 136.8 ms respectively;
root ls returned all 39 immediate entries in 137.6 ms. These are single observed
runs, not steady-state latency percentiles.

A full root `.txt` NUL export completed in **22.0045 s**, returning **1,068,326
paths / 78,196,854 bytes**. Independent streaming duplicate detection confirms
1,068,326 unique paths and a final NUL terminator. The private fixed namespace
provides the independent no-loss oracle; the large run proves completion and
uniqueness beyond both former ceilings, without an independent full tree walk.
The large output is retained on NUC at
`/data1/efs/deploy-pages-abcd9b7b/all-txt-paths.bin`.

Native focused catalog gates pass. Python command/codec gates pass **27 tests**.
Final private direct/TCP command, export, permission, retained-generation,
restart, POSIX and peer gates pass. **Full native `make test` passes**, including all five architecture checks.
Private POSIX passes **212/212** (16.8 s), peer **64/64** (14.4 s).
The live million-file focused command suite also passes: immutable output,
permissions, real .efs collision precedence and slot exhaustion/recovery.
Its descriptor/slot probes use exact legacy matches, separating those
assertions from legacy broad-query limits; broad v4 behavior is validated by
the cap and complete-export runs. An earlier live run with legacy broad probes
hit the legacy cap as expected and is retained as historical failed evidence. These tests do not establish
400 GiB real payload coverage, a hardware CPU ceiling, or full expiry support.

Evidence: [results/measure/20261009-command-pages](../../results/measure/20261009-command-pages/),
with full native logs under `/data1/efs/query-pages-*` and live artifacts under
`/data1/efs/deploy-pages-abcd9b7b` on NUC. The file export is deliberately retained
remotely rather than adding 78 MB of generated paths to Git.
