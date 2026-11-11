# Reader-trial question and answer key for the published group view (Task V2)

**Verdict: NOTRUN, question and key written before any extraction** (Nov 11, 2026). Purpose: a fixed maintenance question and its answer key, committed before any code is moved, so a later reader trial cannot fit the question to the result. Part of the [lifecycle inventory](raft-host-lifecycle-inventory-20261111.md) (Task V2); no source, include or test file changed. Tested tree: `devel` at `68e2778a4`; every line number below is in `src/server/raft_host.c` at that commit. Not determined: how long a reader needs today (nobody has been timed); the trial itself is not run.

## Question

A group's local replica loses leadership (a higher term arrives, or the pump steps it down). At that moment one handler is inside `host_read_index_admitted` holding a view copy taken in the old term, and another handler is asleep in a waiter slot waiting for that group's quorum proof.

1. Which code publishes the change to handlers, and when is it not published?
2. What does each handler observe, and what makes each return instead of using the old-term proof?
3. What wakes the sleeper, and what wakes it if the replica has gone away?
4. Which locks are held at each of those steps, and in which order?

## Answer key

A correct answer contains all of these facts.

1. **One writer of the view.** Only `host_publish_view` (L4345) writes the `v_*` fields. It copies role, term, leader, commit, applied, read and proof state out of the Raft core (L4369-L4380) with `h->mu` held (comment L4344) and stores them (L4399-L4413).
2. **Unchanged state is not published.** It compares every field with the stored one (L4381-L4390) and returns without touching `v_seq` or `v_stamp` when nothing changed (L4395-L4396). A replica that no longer exists (`g->r` NULL) clears `v_has` and the proof fields and bumps the stamp once (L4354-L4367).
3. **Publish is a seqlock window under `view_mu`.** `v_seq` goes odd at L4398, even at L4417, inside `h->view_mu` (L4397, L4418). The stamp is stored last (L4414-L4416), so a waiter that reads the new stamp has the new fields.
4. **Who publishes.** The pump loop at the end of a cycle (L9003-L9005) followed by `host_waiters_wake` (L9006); after a receive-persist batch (L7849-L7851); after an inbox batch handoff (L7899-L7901); `host_stop_leading` after a step-down (L8825); and handlers that start a read round or propose call `host_publish_group` (for example L5576, L5642).
5. **The reader's copy is coherent, and it is only a copy.** `host_view_get` (L4445) retries while `v_seq` is odd or changed (L4453-L4460, L4483-L4484). After `HOST_VIEW_TRIES` (64, L95) odd loads it takes and drops `view_mu` once so a preempted publisher is not starved (L4455-L4459). The handler's `struct host_view v` (L5500) does not change by itself; it sees the new role only on its next `host_view_get` (L5680, and after each wait).
6. **What the in-flight handler does.** On its next loop pass: `!v.has` returns NOT_PRIMARY (L5532); a non-leader role with `proof_only` returns NOT_PRIMARY (L5536-L5538), without it the read is sent to the new leader unless the leader is this node (L5543-L5544); a term different from the admitted term returns NOT_PRIMARY (L5591-L5594); a changed read context returns NOT_PRIMARY (L5596). Independently the proof predicates refuse an old-term proof: `host_view_covers` needs role LEADER and equal term (L4499-L4501), `host_view_quorum_index` and `host_view_wait_apply` need the same (L4512, L4528).
7. **The sleeper.** It registered in a slot with its term and round (`host_waiter_sleep`, L4604-L4616) and sleeps on its own condition variable with `cv_mu` released by the wait. `host_waiters_wake` (L4635), called after the publish, evaluates each active slot under `cv_mu`: for a read waiter (`any` 2 or 3) it calls `host_read_floor_ready` (L4666-L4667), which returns ready when `!has`, the role is not LEADER or the term differs (L4568-L4569, L4542). A slot whose group has `!v_has` wakes unconditionally (L4658-L4659). It marks the slot notified under `cv_mu` (L4672-L4674) and signals after unlocking (L4680-L4681). A sleeper that is not woken leaves by deadline with BUSY (L5650-L5653).
8. **Lock order.** `h->mu` then `view_mu` in the publisher (comment L4344, L4397); `cv_mu` and `view_mu` are never nested; the reader of the view takes no lock except the one empty `view_mu` pass of fact 5; the handler registers under `cv_mu` after re-reading the view under it (L5657-L5658, comment L4575-L4577: that order rules out a lost wakeup).

## Reading set today

The lines a reader must understand today to answer, by range (rerun with the command below):

| Range | What | Lines |
|---|---|---|
| L4345-L4419 | `host_publish_view` | 75 |
| L4421-L4427 | `host_publish_group` | 7 |
| L4437-L4443 | `struct host_view` | 7 |
| L4445-L4485 | `host_view_get` | 41 |
| L4496-L4502 | `host_view_covers` | 7 |
| L4509-L4523 | `host_view_quorum_index` | 15 |
| L4525-L4533 | `host_view_wait_apply` | 9 |
| L4537-L4553 | `host_read_wait_ready` | 17 |
| L4558-L4572 | `host_read_floor_ready` | 15 |
| L4582-L4620 | `host_waiter_sleep` | 39 |
| L4635-L4684 | `host_waiters_wake` | 50 |
| L5495-L5682 | `host_read_index_admitted` | 188 |
| L380-L391 | `struct host_read_floor`, `struct host_waiter` | 12 |
| L340-L357 | the `v_*` fields and their comment in `struct host_group` | 18 |
| L7849-L7851, L7899-L7901, L9003-L9006 | the three pump publish sites | 10 |
| | Total | 510 |

```bash
python3 -c "print(sum(b-a+1 for a,b in [(4345,4419),(4421,4427),(4437,4443),(4445,4485),(4496,4502),(4509,4523),(4525,4533),(4537,4553),(4558,4572),(4582,4620),(4635,4684),(5495,5682),(380,391),(340,357),(7849,7851),(7899,7901),(9003,9006)]))"
```

In the 26,348-line file this is 1.9% of the lines, spread over eight places between L340 and L9006. The seven functions of the group-view concern that the question needs are in one stretch (L4345-L4684, 282 lines) but the consumer (`host_read_index_admitted`) and the publish sites are 1,000 to 4,500 lines away. How the trial will be run: give a fresh reader the question and the file, record the time to a correct answer and which of the eight facts they missed; repeat after an extraction with the owner file, its header and its test. Trial not run.
