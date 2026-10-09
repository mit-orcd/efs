# W97 baseline restoration controls

The rejected c5ff653e partial-reply candidate was drained and replaced with
baseline 4b149e88-batchobs. Diagnostics and virtual commands remain off;
TCP/direct I/O and all data retained. append-control.tsv records 2/2 pass
in 4.6 seconds; identical full core POSIX flags (--jobs 4 --timeout-s 30)
record 212/212 pass in 69.7 seconds. Controls are sequential after namespace
and device-state changes, not randomized matched repeats. The final installed
cluster subsequently advances to group-isolation a770290a-pressure; consult
that experiment's final state. Rollback binaries stay on NUC.
