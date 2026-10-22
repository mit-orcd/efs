# 20260918-w2 — W2 write() durability/visibility measurement

## Purpose and linked item

Measurement run for **W2 — `write()` is specified durable-and-visible;
the code buffers** (closed Sep 18 2026). Linked in
`docs/archive/README.md` (W2 row: "gate:
`results/stress/20260918-w2/`"), `docs/backlog/product-gaps.md` ("peer
0/10, kill -9 loses 64 MiB") and `docs/archive/project-history.md` ("W2
DONE option (i): spec moved — `write()` is client-buffered;
durable+visible at `fsync`/`close`/`O_SYNC`"). The run quantifies the
spec-vs-code gap so the user could choose between (i) moving the spec
and (ii) publishing on `write()`; option (i) was chosen.

## Verdict

**FAIL** (bug confirmed against the then-current spec). Both probes
contradict the specified durable-and-visible contract: a peer never
observed an un-`fsync`ed write (0/10), and `kill -9` of the FUSE daemon
destroyed all 64 MiB of an acknowledged `write()`. The item was later
closed by moving the spec (option i), not by changing the code — this
run is the evidence for that decision. `summary.txt` explicitly records
"Do not implement a fix. User chooses (i) move the spec or (ii) publish
on write()."

## Source commit and build identity

Not recorded in the run — no deployed-version.txt, identity.json, or
version string exists in this directory. (Directory added to the branch
by commit `43e3e4e6a`; archival metadata, not the tested build.)

## Environment

- Cluster: "leftover-1" deployment, `efsd :19810` (fcstor servers, per
  the sibling 20260918-n1-w1 run), TCP transport.
- Clients: fcstor007 (writer, "A") and fcstor008 (peer reader, "B").
- Fixture: (a) a 4 KiB `pwrite` of 0xAA; (b) a 64 MiB `write()` of 'X',
  both without `fsync`/`close`.

## Method

Two probes, recorded in `summary.txt`:

1. **Visibility** — client A `pwrite`s 4 KiB of 0xAA and does not
   `fsync`/`close`; client B `pread`s the same range 10 times over 2 s
   and checks for 0xAA vs ZERO. Result file: `visibility.txt`.
2. **Durability** — client A `write()`s 64 MiB of 'X' and does not
   `fsync`/`close`; `efs-fuse` is killed with `kill -9`; after remount
   the file's size and 'X' byte count are measured. Result file:
   `durability.txt`.

No dedicated script is identified in the run; the probes were driven
ad hoc and only the counter outputs were captured.

## Results

| Probe | Metric | Result |
| --- | --- | --- |
| Visibility | `VIS_HITS` | 0/10 (all ten samples ZERO) |
| Durability | `DUR_SIZE` | 0 |
| Durability | `DUR_X` | 0 |
| Durability | `DUR_MISSING` | 67108864 (64 MiB) |

The create metadata survived the kill; every data byte of the
acknowledged write was lost. Behavior is close-to-open consistency, not
the spec's §3 durable-and-visible `write()`.

## Limitations and remaining gates

- Single probe each (no repetition counts, no timings) — enough to
  demonstrate the gap, not to bound it.
- `O_SYNC` was specified but not wired at this date; wiring it was
  explicitly forbidden as a silent side-cut of a later item.
- Follow-on: after option (i) the spec reads "`write()` is
  client-buffered; durable+visible at `fsync`/`close`/`O_SYNC`";
  publish-on-write (option ii) is forbidden.

## Evidence index

| File | sha256 (first 16) |
| --- | --- |
| `summary.txt` | aefaf152efe49219 |
| `visibility.txt` | 7750658dcd992e17 |
| `durability.txt` | dd9aac0ff529aece |
