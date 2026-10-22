# mac-retained-orphan-runtime.txt — evidence summary

## Purpose and linked item
Runtime identity and service-journal capture for the installed efs1
daemon, taken during the retained-orphan (inode4091) diagnosis. Added by
commit `4124876fc` ("Reproduce retained Mac directory backlink refusal
with safe negative controls", 2026-10-11/12). Linked status page:
`docs/status/mac-retained-orphan-review-20261012.md`.

## Verdict
Informational capture — not a pass/fail run. It records the installed
daemon's process identity, binary hashes and a systemd journal window;
no assertion is evaluated in the file itself.

## Source commit and build identity
No version string is recorded in this file. Recorded identities:
- `efsd` PID 165163, binary sha256
  `2e1851c31c289c26f5fcf908597e003782660e54cdc13fac3c9d54aa51316d36`
- `efs-mgmt` binary sha256
  `5f225931d54bda4bfae1b45a9f422d1a343903a60df1d400359cdc84b45b9de4`

The linked status page identifies the installed daemon as
`e798c2aa`/devel-mac (v0.2.2-pre-alpha lineage).

## Environment
Live single-node deployment on efs1 (Mac host, 192.168.2.11:17432),
`efsd --node-id 1 --storage /data1 --quota 10G --no-direct-io`
(buffered I/O). Run as a systemd-run transient service.

## Method
Read-only inspection of the running deployment: capture service state,
PID and command line, hash the installed binaries, and dump the
`efsd.service` journal for 2026-10-11 ~16:47–19:47 (restart history and
per-incarnation CPU/memory consumption). No service mutation by this
capture itself; the journal shows routine stop/start cycles around it.

## Results
| Field | Value |
|---|---|
| Host / state | efs1 / active |
| Endpoint | 192.168.2.11:17432, buffered (`--no-direct-io`) |
| Quota | 10G on /data1 |
| Journal window | Oct 11 16:47:55–19:47:26, 9 stop/start cycles |
| CPU per incarnation | 14.4s–1min 13.9s |
| Memory peak per incarnation | 299M–718M |

## Limitations and remaining gates
Journal excerpt only; no daemon log content beyond systemd unit lines.
Binary hashes, not a source version string, pin the build. Meaningful
only together with the sibling retained-orphan captures and the review
page; the inode4091 ownership/cause question remains unresolved there.

## Evidence index
- `mac-retained-orphan-runtime.txt` sha256:9dab03ca20b06ae8 — this capture
- `mac-retained-orphan-policy.txt` sha256:c4267ea3c03aa49d — commands-policy probe, same deployment
- `mac-retained-orphan-query-log.txt` sha256:8b060b74dbea3f9b — query refusal log from the same window
- `mac-retained-orphan-current-metadata.txt` — current inode4091/2098/8187 metadata reads
