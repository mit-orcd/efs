# Mac primary mount refresh and revalidation — 2026-10-10

The user explicitly authorized unmounting and losing non-production client data.
`git fetch --all --prune` and the branch ancestry audit found no new commits
outside devel at fee363f5. All compiled source is unchanged from tested/deployed
build 553fbb42d738; fee363f5 contains only testing documentation and evidence.
The recorded refs are in integration-refs.tsv. No new merge was necessary.

The retained gateway clients /mnt/efs and /mnt/efs-b were stopped with the
supported explicit --force-discard option and replaced with build 553fbb42d738.
The primary NFS export was withdrawn during replacement and restored afterward.
The superseded validation mount /mnt/efs-i was stopped normally. All storage
servers and freshly mounted clients run as Linux user efs. No data volume was
wiped. authorized-refresh.log records the completed replacement.

Fresh full core, clean-remount persistence and two-VM peer runs use the documented
Mac correctness profile: core/persistence 600 seconds per case and peer 2400
seconds per step, with original operation counts and assertions retained.
Original performance deadlines remain unmet in earlier evidence; these longer
budgets establish correctness within the declared budget only.

## Fresh validation results

- Native core: 212/212 PASS.
- EFS core: 212/212 PASS, zero failures/skips/unrun cases, 1597.3 seconds.
- Persistence: 26/26 preparation in 282.6 seconds; normal drain/unmount and
  fresh-client mount; 26/26 verification in 113.4 seconds.
- Two-VM peer: 64/64 PASS, zero failures/skips, 1477.9 seconds. The 800-record
  concurrent append step took 1092.24 seconds; the default 500 fsyncs per writer
  are retained.

Every suite TSV has a completion marker and every recorded exit status is zero.
No additional code repair was necessary in this fresh validation.

## Final runtime

Both validation clients stopped normally after the full peer run. The primary
gateway mount was mounted fresh again, and all 300 MiB content bytes and EOF
verified in 4.093 seconds. The primary NFS export is restored.

All three servers and both remaining gateway clients report 553fbb42d738,
with matching installed binary hashes on all four VMs. All EFS processes run
as efs. Cluster health reports 3/3 up, identical versions and idle heal workers.
No retained old clients or pending refresh limitation remains. Final runtime,
health and export state are recorded in runtime-final.log, health-final.log and
export-final.log. The export command succeeded; the following ps returned 1
because no Python test worker remained (header only).
