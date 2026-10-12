# AMD packing-phase diagnostic, 2026-10-12

Diagnostic source: `7ef2cd9bc41a3571801f9f1528cb57eafc71cbde`, e59 plus opt-in timing only. Acceptance source remains e59; this run is not qualification.

Normal build and mounted-pack retry/parallel/batch PASS. Initial live launch failed before creating processes because the output parent directory was missing; its log/results remain retained. Separate live-retry1: seven concurrent clients, jobs16, two original cases (`dir_readdir_listing`, `names_crazy_dirs`), original30s budget, four local daemons, files/TCP/direct, explicit4G per target, both commands flags0 and packed writes1. All14 cases and all7 graceful client stops PASS, client durations4.47–4.67s. Owned diagnostic daemons/clients stopped. No quiet qualification overlapped.

Across seven client logs, admission528 calls totaled1.818s; fragment PUT1584 calls20.548s (median6.666ms,p9543.796ms,max81.662ms); publication/load540 calls1.612s. These are summed phase intervals across concurrent clients, not wall time or isolated close allocation. PUT accounts for85.7% of these three recorded phase totals. Publication/load can execute additional retirement passes, so counts need not match. Unrecorded cohort/busy/retry waits remain possible. This passing restricted workload does not reproduce broad deadline failures or establish the server component responsible.

All23,318 regular source inputs MATCH before and after execution. All74 retrieved evidence files match the remote SHA256 manifest. Runtime config and original worker journals retained. `phase-summary.json` contains aggregate timings and nonzero return codes. Next diagnostic runs the broad original workload with this timing-only build; any implementation must preserve durable ACK, quota, ownership and GC fences.
