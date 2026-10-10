# W97 matched three-node drive comparison — Oct 10

Engine `6b8f05acb484` / `6b8f05acb484-w97`, unchanged production code/settings.
One fresh three-node cluster on NVMe, then one on SATA, 1,200 seconds each.
Eight workers, balanced groups, actual 1–512-byte exclusive CREATE/write/CLOSE;
EFS_MD_RAFT_N=3 with all six hosted group replicas/voters 0x7 asserted. FLOW=1,
10 ms, TCP/direct IO, commands and optional tuning off, diagnostic/perf counts on.
NVMe elections occur around seconds 40–45, then settle on node 1; SATA leadership
is stable. Leader histories and dataset growth sizes differ and are retained.
4,096 prepared selected directories, 1,000 files/directory, 512k cap/worker;
80 GiB physical-free / 3 GiB available-RAM guards. Same fixed 30-second settle
interval after preparation, which takes different times on the two drives.
Regular service stays stopped throughout the measured comparison.

NVMe first/final-minute rates 31.83/10.80; SATA 562.73/299.40 files/s.
Pooled server Raft sync means 2.465/6.993 ms NVMe versus 0.150/0.319 ms SATA.
Whole means 12.91 versus 300.76 files/s, excluding preparation/drain/cold restart.
Final counts 15,506 / 361,060 include the small in-flight tail; zero loader errors.
All replica cuts and 64 sampled exact cold payload checks per drive pass.
No measured sync-error increments. Inbox refusals NVMe n3=35; SATA n2=15/n3=109.

NVMe read-only root health arrives after slowdown begins: 146 samples, no
reported warnings/thermal transitions; composite 34.85–45.85°C, second sensor
32.85–61.85°C. No SATA root telemetry arrives; denied sudo attempt is retained.
Neither telemetry nor host bytes reveal cache occupancy/NAND amplification.
Both drives slow; cause/ceiling unproved. These two models/filesystem paths
are not a general SATA-versus-NVMe protocol ranking; only one run per case.

`nvme/`, `sata/`: raw five-second host/process/Raft snapshots, one-second progress,
perf acknowledged windows, batch histograms/logs, payload latency, placement,
catch-up and cold oracles. `analysis.json` holds all minute/five-minute distributions;
`comparison-summary.json` pools server histogram sums/counts, not node averages.
`baseline-sata.json` reuses the earlier four-node mixed-drive source for context.
`health-samples.jsonl` and `health-summary.json` preserve timestamped root telemetry.
`failed-preflight/` retains default-four-voter assertion refusal before workload and
initial node-4 readiness failure with its passing recovery retry.

Launch provenance hashes freeze workload/controller/perf code. Offline analysis
adds read bandwidth and pooled means after timed tests; offline helper hashes
are separately recorded. Optional root health scripts change no settings.
`tests-exit.txt`, `restore-exit.txt`, `run-exit.txt` are zero. Regular four-node
cluster and both mounts restore with normal default settings and diagnostics off;
final active binary identity is asserted. Both collector stop markers are set.
No private daemon/loader/perf remains. Private storage/data is preserved outside
this evidence folder. Full report: `docs/status/w97-three-drive-20261010.md`.
