# Question routes: symptom questions lead to the owning component (Task V7)

**Current state (Nov 11):** `graphify query "<symptom in plain words>"` now names the owning component on its first output line for the 13 questions in `tools/graphify/routes.json` (4 for `server/group_view`, 3 for `client/put_window`, 2 each for `client/efs_fuse.find_res`, `.writeback` and `.open`). Before the routes the owner was absent or between position 10 and 31 for all of them (absent for 9 of 13). A paraphrase of a route question reaches the owner within 10 output nodes in 5 of 13 cases, so a route helps the question it was written for and its near wording, not any question about the same area. Nothing was changed in the `graphify` program. Linked item: structure review ([structure-review-20261110.md](structure-review-20261110.md)), reader trial 4 ([reader-trials-20261111.md](reader-trials-20261111.md)). Verdict: PASS for the 13 routes and the three tests; the paraphrase result is a limit of the mechanism, below three quarters.

## How `graphify query` picks its start nodes (graphifyy 0.9.97, `serve.py`)

Observed with `graphify query "<text>"` and read in `_query_terms`, `_score_query`, `_pick_seeds` and `_subgraph_to_text`:

1. The text is split into words. Filler words (`what`, `which`, `can`, `how`, `does`, `the`, `for`, `with`, `work`, `get`) and words of two letters or fewer are dropped.
2. Every node is scored on its label (and source path). A word equal to a label scores 1000, a label that starts with the word 100, a label that contains it 1; these are multiplied by the word's rarity. Words in a node's `attributes` or `rationale` add a small amount (0.75).
3. If the whole filtered text equals a node's label, that node scores 10,000 times more and comes first.
4. Start nodes: up to 3 best nodes, then the best node for each remaining word. A word that is also a function name (`read()`, `change()`, `request()`) therefore takes a seed for itself.
5. The graph is walked two steps from all seeds. The output lists seeds first, then the other nodes by distance from the nearest seed and, within a distance, by edge count (most connected first). It stops at about 2,000 tokens (about 60 nodes).

So graph content decides the first line only when a node's label equals the whole filtered question. A component node cannot be that node: its label is its tag (`server/group_view`), and a neighbour of a seed loses the edge-count ranking to test files and headers.

## Mechanism

`tools/graphify/routes.json` lists `{question, component, why}`. `scripts/build-code-graph.py` adds one `question_route` node per entry and one `answered_by` edge to the component node. The node label is the question as graphify filters it (so the identical question equals the label and seeds first). The node carries `attributes` `owner` and `why`; graphify prints attributes on the node's output line, so the first line reads `NODE <question words> [... attrs={owner="server/group_view", why="..."}]`. The component node itself is also in the output but not first; the owner is named by the route line.

## Before and after

`python3 scripts/measure_question_routes.py` prints the after columns (it lists the start nodes too). Position is the first output node whose label is the component tag or its contract header, or whose `owner` attribute is the tag. "Shown" is the nodes printed of the nodes found. Before: the graph at devel `0bffb131d` without routes.

| question | owner | before: owner position (shown) | after: owner position (shown) |
|---|---|---|---|
| what wakes a read waiter after a leadership change | `server/group_view` | 10 (67 of 270) | 1 (65 of 237) |
| which code publishes a role or term change to handlers | `server/group_view` | absent (58 of 460) | 1 (65 of 378) |
| handler keeps using a stale leader view | `server/group_view` | 27 (62 of 501) | 1 (62 of 487) |
| read request sleeps waiting for a quorum proof | `server/group_view` | 10 (66 of 215) | 1 (65 of 219) |
| can a flush wait for a PUT that is still in flight | `client/put_window` | absent (61 of 568) | 1 (58 of 648) |
| what closes a PUT window when the PUT fails | `client/put_window` | 31 (61 of 120) | 1 (57 of 72) |
| reader blocked while a chunk PUT is in progress | `client/put_window` | absent (66 of 211) | 1 (59 of 276) |
| another user sees my find results | `client/efs_fuse.find_res` | absent (44 of 356) | 1 (45 of 354) |
| find query file reads old bytes after the search changed | `client/efs_fuse.find_res` | absent (51 of 586) | 1 (49 of 532) |
| fsync reports an old write error twice | `client/efs_fuse.writeback` | absent (57 of 555) | 1 (57 of 554) |
| write call hangs when many writes are queued | `client/efs_fuse.writeback` | absent (45 of 45) | 1 (45 of 45) |
| file closed but its lease is never released | `client/efs_fuse.open` | absent (69 of 338) | 1 (66 of 352) |
| open of a file answers busy while others hold it | `client/efs_fuse.open` | absent (60 of 332) | 1 (64 of 247) |

Paraphrases (one per route, in `tests/test_code_graph.py`, not in `routes.json`): 5 of 13 reach the owner within 10 nodes (positions 6, 2, 2, 3 and 1; the rest 54 or absent). Hits: "where is a new term or role announced to request handlers", "read stuck until the quorum confirms leadership", "read hangs while a chunk is being uploaded", "after close the file lease stays held", "open returns EBUSY because other openers hold the file". Misses include "which sleeping read gets woken when the leader changes" (54), "does fsync block on a chunk upload that has not finished" and "search results of one user leak to a different user" (absent). The test asserts at least 5.

## What the mechanism cannot do

- It answers the wording it was written for. A different wording seeds on function names and test names; the route node is seeded only when enough of its words match its label.
- Routes do not help an agent find a component that has no route; the component node stays hard to reach through ranking (for `client/put_window` it is at position 41 in a route query).
- A route says where to look, not that the owner behaves as `why` states; `why` repeats the documented contract.
- Routing by symptom words that are also function names (`read`, `change`, `request`) is dominated by those functions.
- Changing this would need a change in graphify's seed choice (for example a query-time alias list); that is the coordinator's decision.

## How to add a route

1. Write the symptom as a person who does not know the component would ask it. Do not use function names.
2. Add `{"question": ..., "component": "<tag>", "why": "<one line from the contract>"}` to `tools/graphify/routes.json`. At most four per component; the tag must exist (`graphify explain "<tag>"`).
3. `sh scripts/rebuild-code-graph.sh`, then `python3 scripts/measure_question_routes.py "<question>" "<tag>"`: the position must be 1.
4. Add a paraphrase to `PARAPHRASES` in `tests/test_code_graph.py` and run `python3 tests/test_code_graph.py`.

## Reproduce

Prerequisites: graphifyy 0.9.97 (see [tools/graphify/README.md](../../../tools/graphify/README.md)). Run `sh scripts/rebuild-code-graph.sh`, `python3 scripts/measure_question_routes.py`, `python3 tests/test_code_graph.py`. Mac only; no cluster. The before columns come from the graph built from the tree before `routes.json` existed (check out `0bffb131d`, rebuild, and run the script with explicit `"<question>" "<tag>"` pairs).
