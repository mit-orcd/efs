# Intel final stop checkpoint — 2026-10-12

Direct human stop relayed by coordinating root: no more tasks, builds, fixtures, pushes or promotion. Intel work stopped after a read-only PUT review. Final read-only /proc check confirms no owned e59 controllers, EFS test processes or private mounts remain. No active native checks remain owned by this chat. Production processes are outside test ownership and were not stopped or redeployed.

## Gates and evidence

- ecdc8c60 focused ordinary I/O, seven clients, jobs16, files/TCP/direct4G, commands client/server0, original30s:70/70 PASS; seven graceful client stops PASS. Evidence8d691139.
- e59e815f focused ordinary I/O plus listing/names:71/84 PASS,13timeoutsFAIL. Ordinary70/70PASS; listingall7FAIL,names6FAIL. All7gracefulstopsPASS; source23318inputs/binaries verified,cleanupidle. Evidencef134cdda.
- First e59 directory observer diagnostic5/14PASS9FAIL, but worker exec bypassed observer; runner journals cannot attribute case phases. Retained38d841d8.
- Corrected exec-surviving worker diagnostic6/14PASS8FAIL; ordinary reference2/2PASS with body sentinels. Listing close/context-exit~24–27s, observed listdir~35–42ms on clients reaching it. Python context exit includes buffered flush/close, not isolated RPC. Retained28c52bdd.
- e59 RPC/read diagnostic4/14PASS10timeoutsFAIL; all7stopsPASS,source/binsunchanged,cleanupidle. Retained70dcd7da37fdf3fe02d5115ff837904b4a654012. Generic client RPC receive aggregates~15–19s exclude manual command/PUT/publication paths; limitation corrected0541b774ff9ec4eaf77e046deaf37574fa5dcc27. These are not full network or close costs.
- Full broad acceptance is FAILED per coordinating AMD report; no alpha acceptance. Intel full TCP/buffered, cross-host redeployment and final full peer/cold/physical matrix are NOT RUN in this assignment. AMD isolated timing-only14/14PASS is reported contextual evidence, not Intel or broad acceptance.

## Read-only PUT review and blockers

No evidence-backed optimization implemented. Missing component measurement: same timing-only diagnostic on Intel, split server authority/session/GC and fragment lock waits, existing read, write/file sync, parent sync count/time, reply, plus client cohort due/busy/retry waits. No requested next measurement was launched before stop. All prior failed evidence retained. Resume only under new human authorization.
