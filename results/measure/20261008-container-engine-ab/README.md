# Experimental daemon engine A/B · 2026-10-08

Branch `devel-storage-engine`, baseline `a42a5be3`. Actual efsd data backend,
TCP fixed, three xorinox storage VMs and one gateway. Private port 27432;
installed port 17432 and gateway `/mnt/efs` processes unchanged before/after.
Measured daemon SHA256:
`d5eb6ef0ec65d1efec63162d97e424715af82766457be5911e891da0eafd4153`.
Runtime C source hashes in `environment/runtime-source-sha256.json` match the
VM build tree. Linux runtime source is in `runtime-source.tar.gz`; the exact
short/sustained measured workload versions are preserved separately. Later
runner changes only add preflight/resume handling and tolerate GC inventory races.

## Results

Eight short functional cases and eight sustained cases passed complete file
hashes, three-daemon restart verification, deletion, and GC convergence to zero
data records and allocated container blocks on every node. Four workers,
256 × 512 KiB files; fsync per create/overwrite. Sustained phases run 30 seconds
with 128 KiB overwrites of 32 hot files. Two alternating/reversed repeats per
engine/I/O mode. MiB/s is client logical throughput including end-to-end EFS costs.

| Engine | Server I/O | Read MiB/s range | Overwrite MiB/s median (range) | Overwrite p99 ms range |
| --- | --- | ---: | ---: | ---: |
| files | buffered | 5.4–5.9 | 25.1 (25.1–25.1) | 39.5–39.6 |
| files | direct | 78.2–139.2 | 25.4 (25.4–25.4) | 39.2–40.2 |
| containers | buffered | 136.3–141.7 | 22.6 (22.5–22.6) | 47.9–50.3 |
| containers | direct | 136.1–137.9 | 22.5 (22.4–22.6) | 49.1–49.5 |

Read measurements are warm end-to-end results. Buffered-files logs show slow
GETCHUNKS metadata read-index waits, including roughly 400 ms busy retries.
Files/direct varies substantially. These results do not isolate raw storage
read speed or establish a general engine ranking. Containers' durable overwrite
throughput is approximately 10–11% lower here; keep files as default.

VMs expose two vCPUs; guest XFS/virtio data disks use qcow2 with QEMU
`cache=none,io=native,iothread=1` on the same host XFS NVMe. Guest direct I/O is
meaningful but does not remove VM scheduling/qcow2/shared-storage effects.
Client settings and transport were fixed. Metadata fragments stay buffered
with both engines. Production load was not stopped or controlled.

## Evidence

- `functional/` and `sustained/`: raw phase JSON, pre/post restart status,
  three-node live/final block inventories, GC status and compressed node logs.
- `summary.json` inside `sustained/`: medians/min/max for throughput,
  latency, duration and operation count. Only two repeats: no confidence claim.
- `installed-before.json` and `installed-after.json`: preserved process snapshots.
- `environment/`: guest/compiler details, host filesystem and VM disk settings.
- `preflight-refusal/`: expected safe rejection of an occupied private test port.
- `sustained/monitoring-note.txt`: observer unlink race, recovery/resume details;
  no measured phase was rerun or silently replaced.

Linux `make -j4 all` and full `make test` passed on the xorinox host. The build emitted zero compiler warnings. Final logs are preserved as `final-build.log.gz` and `final-tests.log.gz`. This is experimental format qualification,
not production acceptance or a power-loss test. Low-level container I/O counters
remain incomplete; physical GC validation uses allocated filesystem blocks.
