# Server command policy evidence

Final code/build fingerprint: `1509e6e1-policy-a9f0a842`.
`source.patch` is relative to branch commit `1509e6e1` and includes the final
C and test changes. `final-fixture/` is the accepted clean-build on/off/on test;
its stores and payload are retained remotely, not copied into Git.

`policy-initial-fstat-failure.log` retains the earlier regression discovery:
reading an old descriptor worked but fstat was rejected with server features
off. It is not passing acceptance. The final fixture proves read/seek/fstat
and existing/cold mount hiding with the repaired genuine-open inode pin path.
`policy-preliminary.log` predates the final clean build and is supporting history.
Build logs, binary hashes and deployment/startup smoke identify the accepted
candidate. Existing NUC data and old binaries were retained during rollout.
