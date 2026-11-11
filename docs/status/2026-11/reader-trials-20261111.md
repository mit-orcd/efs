# Unfamiliar-reader trials: PUT window before and after, group view before (Nov 11, 2026)

**Current state (Nov 11):** three trials run and scored by the coordinator. Verdict: the PUT-window extraction cut what an unfamiliar reader opens by a quarter and irrelevant reading by half, and cut the reading the answer depends on by a tenth. Both readers answered correctly. The group-view "after" trial is NOTRUN until its extraction (Task V3) is merged. Remaining failures: none. Active run: none.

## Why

The structure review of Nov 11 said the reading-set numbers on the [PUT-window page](put-window-ownership-20261110.md) were made by the implementing agent, and asked for an unfamiliar-reader trial that counts validation and correctness. These are the first such trials.

## Method

- **Reader:** a fresh session (Sonnet 5.5, medium effort) with no memory of the code, one per trial.
- **Tree:** a fixed commit per trial; the reader's first command is `git reset --hard <ref>`.
- **Question:** written before the extraction it measures and not changed. PUT window: the question on the [PUT-window page](put-window-ownership-20261110.md). Group view: the question on the [reader-trial page](raft-host-lifecycle-reader-trial-20261111.md).
- **Not allowed:** anything under `docs/status/` (earlier answers), `scripts/evidence/` (trial 3), other commits, the web, other sessions. Allowed: source, headers, tests, the `README.md` guides, `docs/how-it-works/`, and the code graph.
- **Log:** the reader appends one row to `trial/reading-log.tsv` each time text from the tree enters its context (file, first and last line, lines, why), and afterwards classes each row as needed (the answer depends on it), validation (read to confirm) or irrelevant.
- **Score:** the coordinator compares the answer with the committed key, fact by fact.
- **Raw:** the three `trial/` directories (log, answer, summary) are on local refs `keep-trial1` (`2395ab5dd`), `keep-trial2` (`f22f1c985`) and `keep-trial3` (`f0198f4bd`); they are not merged.

## Results

| Trial | Tree | Lines opened | Needed | Validation | Irrelevant | Files | Reads | Key facts right |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1. PUT window, before | `67fc3b4a1` | 1,529 | 878 | 49 | 602 | 9 | 23 | 6 of 6 |
| 2. PUT window, after | `1d93d6965` (wave 80 tree, with Task V1) | 1,148 | 793 | 66 | 289 | 10 | 17 | 6 of 6 |
| 3. Group view, before | `68e2778a4` | 1,167 | 907 | 30 | 230 | 3 | 13 | 6 of 8, 2 in part |

PUT window, after against before: opened 25% less, needed 10% less, validation 35% more (17 lines), irrelevant 52% less, 6 fewer reads.

## What the trials show

- **The implementer's numbers were far too low.** The implementing agent counted 244 needed lines before and 245 after. The unfamiliar readers needed 878 and 793. The key's estimate for the group-view question was 510 lines; the reader needed 907.
- **The extraction removed searching, not reasoning.** This matches the review. The window itself (state, open, close, wait) was found at once in both trees: through the write-path map before, through the owner and its header after.
- **Most of the needed reading is outside the owner.** Both PUT-window readers say the hard half was "when one member has stopped": the bound of the PUT lives in `put_pool_run`, `efs_client_put_fragments_parallel`, `put_fragments_parallel_run` and the RPC deadline, spread over `src/client/write.c`, and neither the graph nor the owner's contract links them to the window. Reader 2 could not confirm the quorum rule.
- **The contract was used.** Reader 2 took the window facts from `put_window.h` and the README and did not open the interface test bodies.
- **Group view:** the reader found publish, wake and the read loop in three contiguous blocks and answered parts 1 to 3; the lock order and the "replica gone" wake were hard, because lock use is spread over the pump, the teardown and the retire path and no comment lists the order.

## Scoring notes

- Trials 1 and 2: every key fact is in both answers (where the window opens, the exits that close it, the broadcast in `put_win_close`, the waiter's bound and four results, the two waiters and 8,000 ms, and the conclusion with the overflow case).
- Trial 3: facts 1, 2, 3, 5, 6 and 8 are right. Fact 4 (who publishes) lacks `host_stop_leading`. Fact 7 (the sleeper) is right for the wake predicate, but the reader concluded that a sleeper whose replica has gone away wakes by timeout, because it found no publish after the replica is removed.
- **The key is wrong in one clause.** Fact 8 of the group-view key says `cv_mu` and `view_mu` are never nested. The reader found, and the [inventory](raft-host-lifecycle-inventory-20261111.md) also says, that `host_waiters_wake` calls `host_view_get` under `cv_mu`, which can take `view_mu` after 64 odd loads. The key page is left as committed; score the later "after" trial with this correction.

## Limitations

- One reader per tree. The difference between trials 1 and 2 includes reader-to-reader variation, which was not measured.
- The log is the reader's own record and was not checked line by line.
- The brief of trial 2 had one sentence more than trial 1 (a fact stated by a contract header or a test may be relied on without reading the implementation).
- "Needed" is the reader's judgement; reader 1 says its count is generous for large ranges where only part mattered.

## Next

- Run the group-view "after" trial on the tree with Task V3 merged, same brief as trial 3.
- The PUT budget (who bounds a PUT and how a stopped member ends it) is what both readers had to dig for. It is a candidate for the next client owner, or at least for a link from the window's guide.
