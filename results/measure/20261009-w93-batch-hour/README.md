# Earlier correctness evidence and superseded controller

Full service POSIX/peer and private read/session/publication/configuration
partition gates were run on c2ab465b. Private checkpoint injected-EIO/cold
replay succeeds on 0bd7b5be (`checkpoint-positive/result.json`). The identical
fixture fails on pre-fix c2ab465b: `checkpoint-negative/marker-observations.json`
records applied.0 advancing 4 to 5 after failed KV sync. No service data was
used by those fault fixtures.

The initial factor controller was interrupted before starting its population
workload to tighten scheduling at the first control/apply fence. Its scripts,
partial baseline startup and `interrupted.json` explain that interruption;
they are not timed performance results. The final method and all four completed
240-second cases are in sibling `../20261009-w93-batch-hour-v2/` with uniform
39be3b38 binaries. Raw private storage contents and rollback binaries are not
copied into the repository.
