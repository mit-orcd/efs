# Structure review task briefs (Nov 10)

**Status (Nov 10): nine briefs prepared; T3, T5, T6, T7 and T8 started Nov 10 as one session each (Sonnet 5.5, medium effort, the user's instruction), with coordinator additions given in each session's prompt (T6: `edelete` for trees, a quiet-host wait and an artifacts directory that is never overwritten; T5: byte-identical output on macOS and Linux; T7: wait for a running merge gate on node9900). T4 starts after T3 is merged; T1 and T2 start after wave 73 is in devel, when the coordinator can convert the open branches; T9 stays deferred. T3, T5, T6 and T8 were merged into devel in wave 73 (Nov 10); T7 is still running.** Source: devel
`cff8eb424`. Findings and order: [structure review](structure-review-20261110.md).
To start a task, give a session the section "Common rules" and the one task
section, verbatim. Each brief is written so that an agent that has not seen
the project can execute it: it names every file to read, every file it may
change, the steps in order, and the checks that prove it is done.

| Task | Title | Kind | Host | Starts when |
| --- | --- | --- | --- | --- |
| T1 | K register: one file per row | docs and one script | none | no open branch edits the checklist, or the merger reruns the converter |
| T2 | Ledger: one file per entry | docs and one script | none | same, for the ledger |
| T3 | Concern map of `src/client/write.c` | docs and one checker | none | any time |
| T4 | Concern map of `src/server/raft_host.c` | docs | none | after T3 is merged |
| T5 | Diagnostics index: log key and error code to owner | script and generated page | none | any time |
| T6 | Test hosts and wave gate in the tree | docs and one script | fstor007 for one trial | any time |
| T7 | Named cases and a case filter in the interface tests | tests | node9900 | no open branch edits the three test files |
| T8 | Test recipe and routing table by component | docs | none | any time |
| T9 | PUT-window ownership in `write.c` | code | node9900 | only after the K38, K102, K106 and K110 fixes are merged |

## Common rules (give these to every session)

1. **Start.** In your worktree run
   `git merge-base --is-ancestor devel HEAD || git reset --hard devel`.
   Your branch must sit on the current `devel`.
2. **Find things with the graph first.** When you know the component, run
   `graphify explain "<component tag>"` (for example
   `graphify explain client/write`). When you do not know the owner, run
   `graphify query "<question>"` to find it, then `explain`. Do this before
   your first grep or source read, and put this rule in every subagent
   prompt that explores code. For a literal log line, `grep -rn "<key>" src`
   is allowed as the first step.
3. **Stay inside your files.** Each task lists the files you may change.
   Change no other file. If the task seems to need another file, stop and
   report; do not decide alone.
4. **Never.** Never push. Never merge into `devel`. Never edit
   `~/git/devops` or `~/git/cluster`. Never read `ACCOUNTS.txt`. Never raise
   a timeout, budget or bound and never add a sleep to make something pass;
   fix the cause or report it. No backward compatibility: a changed format
   or file layout simply replaces the old one.
5. **Live clusters are off limits.** On any host: `efsd` on ports 17432 to
   17435, everything under `/data1/efs` (read only), the `efs` user and its
   mounts, ports 27432 and 27433, `/data1/efs/mnt`,
   `/data1/efs/orcd-mini-mnt`, the `efs-fuse` process of that mount on
   node9900, the stores on fcstor003 to 006 and the `/tmp/efs-mount`
   mounts on fcstor007 to 013. Stop only processes you started, by pid.
   Never `pkill -f`.
6. **Hosts (only where the task names one).** User `erbmi1` behind the jump
   host `orcd-login`:
   `ssh -l erbmi1 -o BatchMode=yes -o ControlPath=none -o ProxyCommand="ssh -o ControlPath='$HOME/git/cluster/state/orcd-mini/.ssh-mux-erbmi1@orcd-login.mit.edu:22' -o BatchMode=yes -W %h:%p orcd-login" <host> '<command>'`.
   If a password or token prompt appears, stop and ask the user; never type
   one. Ship the tree as `git archive --format=tar -o <file>.tar HEAD` by
   `scp` through the same ProxyCommand (three attempts,
   `-o ConnectTimeout=30 -o ServerAliveInterval=15`) and compare `sha256sum`
   on both ends. Work under `/tmp/efs-task<ID>` with
   `TMPDIR=/tmp/efs-task<ID>-tmp`, ports 18000 to 19999 only,
   `SCREENDIR=$HOME/.screen` for `screen`, never `screen -wipe`. libfuse:
   on fstor007 make `/tmp/efs-task<ID>-fuse` with `lib64` and `include`
   symlinks to `~/orcd/scratch/efs/orcd-test/deps/libfuse/lib` and
   `.../include` and export `EFS_FUSE_PREFIX` to it; on node9900 export
   `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`. The shipped tree has no
   `.git`: a script that calls `git` must also work without it. Remove
   `/tmp/efs-task<ID>*` and the tar when done. Raw output stays outside Git
   under `~/efs-artifacts/task<ID>-<yyyymmdd>/` with a `SHA256SUMS` file.
7. **Record.** Commit one evidence page
   `docs/status/2026-11/<name>-<yyyymmdd>.md` that opens with one status
   block (source commit, verdict, remaining failures, active run, next
   action, merge state: your branch name and "unmerged"), then purpose,
   what changed, commands run with results, limitations. Add one ledger
   paragraph: in `docs/status/2026-11/ledger.md`, directly under the line
   `## Recent evidence and remaining acceptance`, as the first entry, in
   this form:

   ```text
   [Title](<name>-<yyyymmdd>.md): **verdict, no K row, blocks alpha: no** (date, branch). One or two sentences.
   ```

   (If Task T2 has
   merged, add a file under `docs/status/2026-11/ledger/` instead, as its
   README says.)
8. **Check before you commit.** Run and make pass:
   `python3 scripts/check_doc_links.py`, `python3 scripts/status_layout.py`,
   `make docs-check`, `python3 scripts/check_k_register.py`. Then rebuild
   the graph: `sh scripts/rebuild-code-graph.sh` and
   `python3 tests/test_code_graph.py`.
9. **Commit.** Stage files by name; never `git add -A`, `git commit -a` or
   `git stash`. Commit the graph as its own last commit:
   `git add -f graphify-out/GRAPH_REPORT.md graphify-out/graph.json graphify-out/source-manifest.json graphify-out/graph.html`
   (this `add` may exit 1 although it worked; confirm with `git status`),
   then commit. End every commit message with the attribution line your
   session was given.
10. **Report.** First line: branch and commit ids. Then the done checks with
    their results, then what you did not do and why.

---

## T1. K register: one file per row

**Why.** `docs/status/alpha-1.0-2plus1-checklist.md` is 376 KB; its two K
tables (header `| ID | Item | Kind | Disposition |` near line 120 and header
`| Row | Finding on devel |` near line 244) hold one very long line per K
row in each table. Every fix task must read and edit them, parallel
branches conflict in them, and K numbers collide.

**You may change:** `docs/status/alpha-1.0-2plus1-checklist.md`,
`scripts/check_k_register.py`, `tests/test_k_register.py`, new files
`docs/status/k/K<NNN>.md` and `docs/status/k/README.md`, new
`scripts/evidence/t1/split_k_register.py`, your evidence page, the ledger.

**Read first:** `scripts/check_k_register.py` (106 lines) and
`tests/test_k_register.py`; the two table header lines and three rows of
each table (do not read the whole checklist).

**Steps.**

1. Write `scripts/evidence/t1/split_k_register.py`. It reads the checklist
   and, for every K number, writes `docs/status/k/K<NNN>.md` (three digits,
   `K001.md`) with exactly this layout:

   ```text
   # K<N>: <title>

   - State: <FIXED|VERIFIED|VERIFY|DECIDED|CLOSED|OPEN>
   - Blocks alpha: <yes|no|unstated>
   - Kind: <the Kind cell of the first table>

   ## Item

   <the Item cell of the first table, unchanged>

   ## Disposition

   <the Disposition cell of the first table, unchanged>

   ## Finding on devel

   <the second table's cell for this row, unchanged>
   ```

   Rules, all mechanical: `<title>` is the Item cell with Markdown `**`
   removed, cut at the first of `. `, `: ` or 100 characters. State is the
   first word of the Disposition cell (the existing script already checks
   that both tables agree). "Blocks alpha" is `yes` or `no` if either cell
   contains `blocks alpha: yes` or `blocks alpha: no` (case-insensitive;
   if both occur take the one in the second table), else `unstated`.
   Relative links: every `](target)` whose target does not start with
   `http`, `#` or `/` gets `../` put in front, because the row file is one
   directory deeper. The script must be rerunnable: running it twice gives
   identical files.
2. Change `scripts/check_k_register.py` so that the row files are the
   source. It must: fail if `docs/status/k/` does not hold exactly one file
   for each number from K1 to the highest, with no gap and no duplicate;
   fail if a State line is not one of the six words; generate the register
   table

   ```text
   | ID | Title | Kind | State | Blocks alpha | Row |
   | --- | --- | --- | --- | --- | --- |
   | K1 | <title> | <kind> | <state> | <yes/no/unstated> | [K001](k/K001.md) |
   ```

   and a counts line
   `Current counts: <n> FIXED / <n> VERIFIED / <n> VERIFY / <n> CLOSED / <n> DECIDED / <n> OPEN (<total> rows).`;
   with `--write` put both into the checklist between the marker lines
   `<!-- K-REGISTER:BEGIN (generated by scripts/check_k_register.py --write) -->`
   and `<!-- K-REGISTER:END -->`; without `--write` exit 1 when the text
   between the markers differs from what it would generate. Keep the
   `--json` output and the last line `PASS K register: ...` (other scripts
   read it).
3. In the checklist: replace the first K table by the two marker lines
   (then run `--write`); delete the second table and put one sentence in
   its place:

   ```text
   Each row's finding is in its file under [k/](k/README.md).
   ```

   Leave the long "Current counts (" sentence near line 351 as it is but
   put "Wave notes (historical; the counts above are generated): " in
   front of it. Change nothing else in the checklist.
4. Write `docs/status/k/README.md` (under 25 lines): the file layout above,
   "to add a row, copy the layout into the next free number and run
   `python3 scripts/check_k_register.py --write`", "to change a state,
   edit the State line and run the same command", and "two branches that
   add the same number: the later merge renames its file to the next free
   number and reruns the command".
5. Update `tests/test_k_register.py` to the new source: it must test a
   missing number, a duplicate number, a bad State word, and a stale
   generated table (each must make the check fail), on temporary copies.

**Done when.** `python3 scripts/check_k_register.py` prints the PASS line
with the same six counts as before your change (record both lines on your
page); `python3 tests/test_k_register.py` passes;
`python3 scripts/check_doc_links.py` reports 0 dangling links; the
checklist is under 170 KB (`wc -c`); running the splitter a second time
changes nothing (`git status` clean after it); your page states the K row
count and the byte size before and after.

**If something is unclear.** A cell that the splitter cannot parse (a row
with other than 4 or 2 cells) is reported by the existing check: stop and
report the row number.

**For the merging session.** Open branches that edited the old tables are
converted by checking out their checklist, running the splitter on it and
taking the resulting row files.

---

## T2. Ledger: one file per entry

**Why.** `docs/status/2026-11/ledger.md` is 172 KB: 101 entries, each one
very long line, all under the heading `## Recent evidence and remaining
acceptance`, newest first. Every branch inserts at the same place, so every
merge conflicts.

**You may change:** `docs/status/2026-11/ledger.md`, new directory
`docs/status/2026-11/ledger/` (entry files and `README.md`), new
`scripts/gen_ledger.py`, `Makefile` (one line in the `docs-check` recipe),
your evidence page.

**Read first:** lines 1 to 8 of the ledger and its first three entries.
An entry is one non-empty line after the heading. It normally starts with
a Markdown link to its page, a colon and a bold verdict; a few start with
"Task XX, " before the link:

```text
[Title](page.md): **verdict** ...
Task XX, [Title](page.md): **verdict** ...
```

**Steps.**

1. Write `scripts/gen_ledger.py` with three modes.
   - `--split` (run once): for each entry line write
     `docs/status/2026-11/ledger/<yyyymmdd>-<slug>.md` containing
     `# <title>`, a blank line, and the entry line unchanged except that
     every relative link target (not starting with `http`, `#` or `/`)
     gets `../` in front. `<title>` is the text of the first `[...]` link
     of the line. `<slug>` is the file name of that link's target without
     `.md` and without a trailing `-2026MMDD`; if two entries give the same
     file name, append `-2`, `-3`. `<yyyymmdd>`: the first `Nov <d>` in the
     first 400 characters of the line gives `202611<dd>`; if there is none,
     use the date of the entry directly below it (older); the last entry
     without a date gets `20261101`.
   - `--write`: regenerate `ledger.md`: keep everything up to and including
     the heading line, then one line per entry file, newest file name
     first, in this form (the verdict is the first bold span of the entry,
     or nothing):

     ```text
     - [<title>](ledger/<file>): **<verdict>**
     ```

   - no argument: exit 1 if `ledger.md` differs from what `--write` would
     produce, printing the first differing line.
2. Run `--split`, then `--write`. Check that the number of files equals
   the number of entries (101 at `cff8eb424`; count yours first with
   `awk '/^## Recent evidence/{f=1;next} f&&NF' docs/status/2026-11/ledger.md | wc -l`).
3. Add `python3 scripts/gen_ledger.py` as a line in the `docs-check`
   recipe of the Makefile (after `check_doc_links.py`).
4. Write `docs/status/2026-11/ledger/README.md` (under 20 lines): "one
   file per entry; to add an entry create `<yyyymmdd>-<slug>.md` with a
   `# Title` line and one paragraph (links relative to this directory, so
   a status page is `../page.md`), then run
   `python3 scripts/gen_ledger.py --write`; two branches never touch the
   same file; order inside one day is by file name."

**Done when.** `python3 scripts/gen_ledger.py` exits 0; `ledger.md` is
under 25 KB; `python3 scripts/check_doc_links.py` reports 0 dangling
links; the concatenated text of the entry files, with the `../` prefixes
removed again, contains every original entry line (write this comparison
as a ten-line check in your page's method and show its result);
`make docs-check` passes.

**For the merging session.** A branch that added an entry to the old
`ledger.md` is converted by moving its added line into a new entry file.
The ledger conflict resolver used in earlier merges is no longer needed.

---

## T3. Concern map of `src/client/write.c`

**Why.** `src/client/write.c` is 9,812 lines with about 289 functions, 30
file-scope state items and 9 mutexes. Its guide (`src/client/README.md`
line 6) says only that it "owns the dirty cache". K38, K102, K106, K110
and K32 are all in this file. An agent with a defect there has no map.

**You may change:** new `docs/how-it-works/write-path-map.md`,
`src/client/README.md` (add one pointer line and one table), new
`scripts/check_concern_map.py`, `Makefile` (one line in `docs-check`), your
evidence page, the ledger. **Do not change `src/client/write.c`** or any
other source or test: other sessions are fixing defects in it now.

**Read first:** `graphify explain client/write`; `src/client/README.md`;
`src/client/client_internal.h` (899 lines; it declares what other client
files use from `write.c`); then `write.c` itself, one concern at a time.

**Steps.**

1. List every function defined in `write.c` (drop lines that start with
   `if`, `for`, `while`, `switch`, `return`, `else`) and the file-scope
   state:

   ```sh
   grep -nE '^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z0-9_]+)\([^;]*$' src/client/write.c
   grep -nE '^static [^(]*(=|;)$' src/client/write.c
   ```

2. Group the functions into concerns. Start from these and adjust to what
   the code shows: dirty-cache slots and bodies (`efs_dcache_*`,
   `dcache_*`), pinning and reclaim (`dcache_pin*`, `dcache_reclaim*`),
   the PUT window (`put_win_*`, `put_pool_*`), PUT ids and fragment sends,
   REPORT and publication units (`report_*`, the section that starts at the
   banner "W77 · atomic publication units"), write-back error state
   (`wb_error*`, `efs_wb*`), flush pipeline (`dcache_flush*`,
   `flush_pipe*`), dirty snapshots (`dirty_snap*`, `dirty_set*`), export
   and fold (`export_chunk*`), node notes (`node_note*`), public entry
   points (`efs_client_*`). A function belongs to exactly one concern.
3. Write `docs/how-it-works/write-path-map.md`. One `## <Concern>` section
   per concern, each with exactly these parts:
   - "Does:" two or three sentences, from the code.
   - "State:" the file-scope variables and struct fields it owns, each
     with the mutex that protects it (name the mutex variable).
   - "Entry points:" the functions other concerns or other files call.
   - "Functions:" a single line starting `Functions:` followed by every
     function of the concern in backticks, separated by spaces.
   - "Logs:" the diagnostic keys it prints (for example
     `dcache-flush-fail`), found with
     `grep -n 'fprintf(stderr' src/client/write.c` in its line range.
   - "Tests:" up to five test files that exercise it, from
     `python3 scripts/tests_for.py client/write` and
     `grep -l "<function>" tests/*.py tests/*.c`.
   Then one section `## Lock order` listing the order in which the mutexes
   are taken where two are held at once, each with one function that shows
   it (function name, not line number). Do not put line numbers anywhere
   on the page: they drift.
4. Write `scripts/check_concern_map.py MAP.md SOURCE.c [SOURCE2 ...]`: it
   collects the function definitions of the source files (the pattern of
   step 1) and the backticked names on the `Functions:` lines of the map,
   and exits 1 listing (a) defined functions that are in no concern, (b)
   names in the map that are not defined, (c) names that appear in two
   concerns. It must not call `git`. Add
   `python3 scripts/check_concern_map.py docs/how-it-works/write-path-map.md src/client/write.c`
   to the `docs-check` recipe.
5. In `src/client/README.md` add, under the line about `write.c`, a
   pointer to the map and a table "Symptom | Concern" with one row per
   diagnostic key or user-visible symptom you met in step 3 (for example
   "`dcache-flush-fail stage=put` | flush pipeline").

**Done when.** `python3 scripts/check_concern_map.py ...` exits 0, and
exits 1 when you temporarily delete one name from a `Functions:` line
(show both on your page, then restore); the map is under 400 lines;
`make docs-check` passes; `python3 scripts/check_guide_counts.py` still
passes; your page records, for the question "why can `close()` return EIO
after a member stops while a PUT is in flight", which sections of the map
you would open and how many lines of `write.c` they point at, against the
9,812 lines of the file.

**If something is unclear.** If you cannot tell which mutex protects a
variable, write "not determined" for it; do not guess.

---

## T4. Concern map of `src/server/raft_host.c`

**Starts after T3 is merged** (it uses `scripts/check_concern_map.py`).

**Why.** `src/server/raft_host.c` is 26,249 lines with about 655 functions.
Only the ReadIndex part has a map
(`docs/status/2026-11/readindex-entry-map-20261108.md`, 32 functions). That
map found that functions must be grouped by what they do, not by their
name prefix.

**You may change:** new `docs/how-it-works/raft-host-map.md`,
`src/server/README.md` (one pointer line and one symptom table),
`Makefile` (one line in `docs-check`), your evidence page, the ledger.
**Do not change `raft_host.c`** or any other source or test.

**Read first:** `graphify explain server/raft_host`; `src/server/README.md`;
the "Findings the split must act on" and "Entry map" sections of the
ReadIndex map; `docs/how-it-works/apply-visibility.md` (the apply scope);
section 2b of `docs/status/2026-11/structure-plan-20261102.md`.

**Steps.** The same six-part section layout, the same rules (no line
numbers, one concern per function, "not determined" instead of a guess)
and the same checker as T3, applied to `raft_host.c`. Start from these
concerns and adjust: group and view state (`host_view_*`, `host_member_*`),
the pump and outbox (`host_pump*`, `host_send*`), apply (`host_apply*`),
ReadIndex round (take the 17 functions the existing map marks as the
round, the pair path and the apply wait; the other 11 prefixed functions
go to the concern the existing map names for them), append
(`host_append_*`), publication (`host_pub_*`, `host_publish*`), forwards
(`host_fwd_*`), GC and reclaim (`host_gc_*`), transactions and prepare
(`host_txn_*`, `host_prep_*`), sessions (`host_session_*`), expiry
(`host_expiry_*`), reports (`host_report_*`), snapshots (`host_snap_*`),
query (`host_query_*`, `query_page_*`), namespace and directories
(`host_namespace_*`, `host_dir_*`), KV access (`host_kv_*`), packing
(`host_unpack_*`), proposals (`host_propose_*`), public entry points
(`server_raft_host_*`, 63 functions: put each under the concern it serves
and list it in that concern's "Entry points"). Work through the file in
order, 2,000 lines at a time, and run the checker after each part; it
tells you which functions are still unmapped. Add the `docs-check` line
`python3 scripts/check_concern_map.py docs/how-it-works/raft-host-map.md src/server/raft_host.c`.
In `src/server/README.md` add the pointer and a "Symptom | Concern" table
from the diagnostic keys you met.

**Done when.** The checker exits 0 for `raft_host.c`; the map is under 900
lines; `python3 scripts/evidence/do/check_entry_map.py` still passes (you
did not touch its page); `make docs-check` passes; your page records, for
the question "which functions run between a metadata batch being staged
and a derived counter seeing it", the sections you would open and the
number of functions they list, against 655.

---

## T5. Diagnostics index: log key and error code to owner

**Why.** Defects arrive as a log line such as
`dcache-flush-fail ino=6980 stage=put rc=-3`. `graphify query` on that
line returns 613 unrelated nodes; `grep` finds the source in one call; the
meaning of `rc=-3` is only in `include/efs/common.h` (lines 111 to 155).

**You may change:** new `scripts/gen_diagnostics.py`, new generated
`docs/how-it-works/diagnostics.md`, `AGENTS.md` (two sentences in the
section "Canonical defect-navigation graph"), `Makefile` (one line in
`docs-check`), your evidence page, the ledger.

**Read first:** `include/efs/common.h` lines 105 to 160;
`scripts/tests_for.py` (how a file path maps to a component tag; reuse its
function for that, import it by path as `tests/test_tests_for.py` does);
ten `fprintf(stderr, "..."` lines in `src/client/write.c` to see the key
style.

**Steps.**

1. `scripts/gen_diagnostics.py` scans every `.c` and `.h` under `src/` and
   `include/` for string literals passed to `fprintf(stderr, ...)` and the
   project's log helpers (find them with
   `grep -rhoE '\b[a-z_]*log[a-z_]*\(' src | sort | uniq -c | sort -rn | head`;
   name on your page which helpers you included). The diagnostic key is
   the leading token of the literal when it has the form
   `word-word` (lower-case letters, digits and at least one hyphen)
   followed by a space or `:`. For each key record every file and the
   enclosing function (the nearest function definition above the line).
2. It writes `docs/how-it-works/diagnostics.md` with: a first line saying
   the page is generated and by which command; section `## Error codes`,
   a table `| rc | Name | Meaning |` from the `#define EFS_ERR_*` lines of
   `common.h` (meaning = the comment on the line if there is one, else
   empty); section `## Diagnostic keys`, a table
   `| Key | File | Function | Component |` sorted by key, one row per
   (key, function), Component being the tag the file maps to.
   No line numbers. With `--check` it exits 1 if the page on disk differs
   from what it would write. It must not call `git`.
3. Add `python3 scripts/gen_diagnostics.py --check` to `docs-check`.
4. In `AGENTS.md`, after the sentence that begins "Component known: start
   with", add: "A literal log line or error code: look the key up in
   `docs/how-it-works/diagnostics.md` (or `grep -rn '<key>' src`), then
   `graphify explain` the component it names."

**Done when.** The page lists `dcache-flush-fail` with file
`src/client/write.c` and component `client/write`, and `-13` as
`EFS_ERR_BUSY`; `--check` exits 0, and exits 1 after you change one
character in the page (show both, then regenerate); the page is under
40 KB; `make docs-check` passes; your page gives the number of keys and
of error codes found.

**If something is unclear.** A message without a key of that form is not
listed; state on your page how many `fprintf(stderr` calls have no key.

---

## T6. Test hosts and the wave gate in the tree

**Why.** How to reach the test hosts, build there, run the gate and clean
up is written in no tracked file; it travels in task briefs and is
re-derived for every merge. Sub-sessions lost time on it repeatedly.

**You may change:** new `docs/how-it-works/test-hosts.md`, new
`scripts/wave_gate.sh`, `docs/how-it-works/testing.md` (one pointer line
at the top of the section "Linux build prerequisites"), your evidence
page, the ledger. No secrets, no account names other than `erbmi1`, and
never read `ACCOUNTS.txt`.

**Read first:** rules 5 and 6 of "Common rules" above (they are the
content to move into the page);
`docs/status/2026-11/k38-distinct-host-rdma-20261107.md` (the fcstor
private-cluster recipe); `docs/status/2026-11/fuse-prefix-build-20261109.md`;
`scripts/make_test_summary.sh` (what it writes: `summary.txt`,
`timings.tsv`, `build.log`, `progress.txt`, `tmp/fail-*.log`).

**Steps.**

1. Write `docs/how-it-works/test-hosts.md`, under 150 lines, with these
   sections: "Access" (the ProxyCommand, the stop-and-ask rule for a
   password prompt, the three-attempt `scp`); "Hosts" (a table: fstor007,
   96 CPUs, no valgrind header, libfuse by shim; node9900, 192 CPUs, no
   valgrind, `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, also carries the
   live orcd-mini client; node9901, live orcd-mini daemons, use last;
   fcstor003 to 015, shared home, private clusters only, fcstor015
   refuses the key; the home directory is shared by all of them);
   "Never touch" (rule 5); "Work layout" (`/tmp/efs-task<ID>`, `TMPDIR`,
   ports, `SCREENDIR`, artifacts directory with `SHA256SUMS`, cleanup);
   "A shipped tree has no `.git`"; "The wave gate" (the command of step 2,
   what COMPLETE, FAIL, NOTRUN host limit and OVERBUDGET mean in the first
   line of `summary.txt`, and the rule for a failure of a known
   intermittent: keep the artifacts, run the test alone three times on
   the same build, run the gate once more, never widen a budget).
2. Write `scripts/wave_gate.sh` from the script below, which is the one
   used by hand for waves 60 to 68. Usage:
   `scripts/wave_gate.sh <host> <tar-on-host> <name>` run **on the host**;
   it extracts the tar to `/tmp/efs-<name>`, sets the libfuse prefix by
   host name, runs the gate detached in `screen`, and copies the results
   to `~/efs-artifacts/<name>-<host>/` with `SHA256SUMS`. A second mode
   `scripts/wave_gate.sh --clean <name>` removes `/tmp/efs-<name>*`.

   ```sh
   set -u
   export SCREENDIR=$HOME/.screen; mkdir -p $SCREENDIR; chmod 700 $SCREENDIR
   D=/tmp/efs-<name>
   rm -rf $D $D-tmp $D-fuse; mkdir -p $D $D-tmp
   tar -C $D -xf <tar-on-host>
   # fstor007 only: mkdir -p $D-fuse
   #   ln -s $HOME/orcd/scratch/efs/orcd-test/deps/libfuse/lib $D-fuse/lib64
   #   ln -s $HOME/orcd/scratch/efs/orcd-test/deps/libfuse/include $D-fuse/include
   cat > $D/gate-run.sh <<'INNER'
   cd /tmp/efs-<name> || exit 2
   export TMPDIR=/tmp/efs-<name>-tmp; mkdir -p $TMPDIR
   export EFS_FUSE_PREFIX=<$D-fuse on fstor007, /data1/efs/deps/libfuse on node9900>
   EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > gate.log 2>&1
   A=$HOME/efs-artifacts/<name>-<host>; mkdir -p $A
   for f in summary.txt timings.tsv build.log progress.txt gate.log; do [ -f $f ] && cp $f $A/; done
   cp -r tmp/fail-*.log $A/ 2>/dev/null
   cd $A && sha256sum * > SHA256SUMS
   INNER
   screen -dmS <name> bash $D/gate-run.sh && sleep 2 && screen -ls | grep <name> && echo started
   ```

   The gate is finished when `~/efs-artifacts/<name>-<host>/SHA256SUMS`
   exists; the result is the first line of `summary.txt`.
3. Trial on fstor007 (check `uptime` first; if the load is above 20, wait
   and say so): ship a tar of your branch, run
   `scripts/wave_gate.sh fstor007 ~/<tar> taskT6`, wait for `SHA256SUMS`,
   record the first line of `summary.txt`, run `--clean`, remove the tar.
4. Add the pointer line in `testing.md`.

**Done when.** `bash -n scripts/wave_gate.sh` passes; the trial's first
summary line is on your page (a FAIL of a test is recorded with its name,
it is not yours to fix); the page exists and `make docs-check` passes;
`ls /tmp/efs-taskT6*` on fstor007 shows nothing.

---

## T7. Named cases and a case filter in the interface tests

**Why.** `tests/test_wb_interface.c` (279 lines), `tests/test_open_interface.c`
(723) and `tests/test_find_res_interface.c` (379) run all their checks
inline in `main`. A reader cannot run or name one case. The seven sequence
binaries already have a filter (see how `tests/test_session.c` does it:
a local `RUN(...)` macro, `--list`, exit 2 for an argument that matches no
case).

**Before you start:** run
`git branch -a --contains devel | head -30` and, for each branch not
merged, `git diff --name-only devel...<branch> | grep -E 'test_(wb|open|find_res)_interface'`.
If any open branch edits one of the three files, do the other files only
and report which you skipped.

**You may change:** the three test files, the case-map tables in
`src/client/efs_fuse/README.md` (the tables under "Operation or
invariant | Cases | Command"), `docs/how-it-works/testing.md` (extend the
existing sentence that lists the binaries with `--list`), your evidence
page, the ledger. Do not change any file under `src/` other than that
README, and not `tests/budgets.tsv`.

**Read first:** `tests/test_session.c` from `main` to the end and its
`RUN` macro; `docs/status/2026-11/test-case-filter-20261109.md`;
`docs/status/2026-11/contract-case-map-20261109.md`.

**Steps, for each of the three files.**

1. Cut the body of `main` into `static void test_<what>(void)` functions,
   one per check group (a group is the lines between two comment headers
   or blank-line blocks that test one operation). Move the lines without
   changing them. Shared setup that every group needs stays in a
   `static void setup(void)` called at the start of each case if the
   groups depend on it; if a group depends on state left by the group
   before it, keep those two in one case function and say so in a
   one-line comment.
2. Give `main` the same argument handling as `tests/test_session.c`: no
   argument runs every case in the old order; an argument runs the cases
   whose name contains it; `--list` prints the names; an argument that
   matches nothing exits 2 and prints the names to stderr.
3. Build and run on node9900: `make tests/test_wb_interface
   tests/test_open_interface tests/test_find_res_interface` then each
   binary with no argument, with `--list`, with one case name, and with a
   misspelled name; then `make test`; then the ASan and TSan lanes for the
   three (`tests/sanitizer_check.sh asan` and `tsan`; record the three
   entries' lines).
4. Update the case-map tables: every row that says a case is "inline in
   `main`" now names its `test_...` function and the command
   `./tests/<binary> <case>`.

**Done when.** For each binary: bare run rc 0 with the same number of
checks as before (count the `assert` lines moved: none added or removed,
`grep -c 'assert(' <file>` equal before and after); `--list` prints the
case names; a misspelled name exits 2; `python3 scripts/check_guide_counts.py`
passes (it verifies every case named in the tables exists);
`make test` rc 0 on node9900; both sanitizer lanes PASS for the three
entries; your page has a table binary, cases, checks.

---

## T8. Test recipe and routing table by component

**Why.** `tests/` holds 625 files and no README. Adding a test today means
finding an earlier task page to imitate. `docs/how-it-works/developing.md`
section 2 ("Routing: I am changing X") routes by topic to protocol pages,
names no component tag, and states things that are no longer true.

**You may change:** new `tests/README.md`, `docs/how-it-works/developing.md`
(section 2 only, and the two stale sentences named in step 3), your
evidence page, the ledger.

**Read first:** `docs/how-it-works/testing.md` lines 110 to 230 (the
`make test` section and the catalogue text); the header comment of
`tests/budgets.tsv` (its first 30 lines); `tests/test_open_interface.c`
lines 1 to 60 and its row in `tests/budgets.tsv`; `tests/k33_daemons.py`
lines 1 to 60 (the helper that starts private daemons);
`tests/test_k96_status_slots_live.py` (a short live test);
`scripts/tests_for.py` (usage: `tests_for.py SUBSYSTEM [--run]`; it has no
`--help` and no `--list`).

**Steps.**

1. Write `tests/README.md`, under 90 lines: "Which kind of test" (a table
   with three rows: an interface test in C that links the real module
   through its header; a Python test that starts private daemons with
   `tests/k33_daemons.py`; a sequence binary case), each with the example
   file to copy, where to add the build rule (name the Makefile variable
   the example uses; find it with `grep -n test_open_interface Makefile`),
   and "when to use". Then "Register it": the `tests/budgets.tsv` row
   (the columns, in order, taken from the file's header comment; budget =
   1.25 times the slowest of three runs alone, rounded up to 5 s; the
   component tag or tags, comma-separated), `make test` versus
   `make test-long` (new tests go into `make test`), the sanitizer lists
   in `tests/sanitizer_check.sh` for a C binary. Then "Run it":
   `python3 scripts/tests_for.py <tag>` and `--run`, the case filter of
   the sequence binaries, `python3 tests/test_make_test_recipe.py` and
   `python3 scripts/check_guide_counts.py` as the checks that the
   registration is consistent. Every command you write must be one you
   ran; paste none you did not run.
2. In `developing.md` section 2, add a column "Component" to the routing
   table: for each row the component tag or tags an agent passes to
   `graphify explain` and `scripts/tests_for.py` (get the list of tags
   from the sixth column of `tests/budgets.tsv`:
   `grep -v '^#' tests/budgets.tsv | cut -f6 | tr ',' '\n' | sort -u`). A row for which
   no tag fits gets "none". Put one sentence above the table: "Start with
   `graphify explain <Component>`; the Read column is what to read next."
3. In the same file fix two stale statements and nothing else: the row
   "Module structure / file layout" says "~1000-line file cap": replace
   the Governs cell with this text:

   ```text
   owner interfaces: private state behind a small header ([structure plan](../status/2026-11/structure-plan-20261102.md))
   ```

   in section 5 the sentence about "make test-all passes ... via
   tests/run_tests.sh": check that `tests/run_tests.sh` exists and what it
   runs; if the sentence is still true leave it, if not, replace only the
   false part and say on your page what was false.

**Done when.** `tests/README.md` exists and every command in it is listed
on your page with its exit code; the routing table has the new column
with a tag or "none" in every row and every tag is accepted by
`python3 scripts/tests_for.py <tag>` (exit 0 or 1, never 2; list them);
`make docs-check` passes.

---

## T9. PUT-window ownership in `src/client/write.c` (deferred)

**Do not start until** the fixes for K38, K102, K106 and K110 are merged
into `devel` (ask the coordinating session), and T3 is merged (its map
names the PUT-window concern's functions and state).

**Why.** The PUT window (`put_win_mu` and the `put_win_*` functions, about
line 5217) is shared state inside a 9,812-line file where the campaign's
write-path defects sit. The three ownership pilots (write-back pool, open
table, `.find` cache) each made a single-owner question answerable from
one guide, one header and one file.

**You may change:** `src/client/write.c`, new
`src/client/put_window.c` and `src/client/put_window.h`,
`src/client/client_internal.h` only to remove declarations that move,
the Makefile (object list and the new test), new
`tests/test_put_window_interface.c`, `tests/budgets.tsv` (one new row),
`tests/sanitizer_check.sh` (both lists), `tests/csrc.py` (the `'write'` entry of
its `MODULES` table, so the splice tests still concatenate the moved code),
any `tests/test_*.py` that splices a moved symbol
(`grep -l put_win tests/`), `src/client/README.md`,
`docs/how-it-works/write-path-map.md`, your evidence page, the ledger.

**Read first:** `docs/status/2026-11/find-res-ownership-20261108.md` and
`docs/status/2026-11/taskEB-review-followup-20261108.md` (the recipe, step
by step); `docs/status/2026-11/reading-set-baseline-20261104.md` section
"Measures"; the PUT-window section of the write-path map.

**Steps.**

1. Before any source change, commit a "before" page section: answer the
   question "can a PUT wait in the window past the flush bound when one
   member has stopped, and what wakes it" from the tree, and record lines
   needed and opened for understanding and for validation, and source
   files opened, as the Measures section defines them.
2. Move the window's state, its mutex and the `put_win_*` functions into
   `put_window.c`, unchanged. `put_window.h` declares only the operations
   other code calls and carries, at its top, the contract: who owns the
   state, which lock, what each operation guarantees, lock order against
   the dirty-cache mutexes. No behavior change, no reordering inside a
   function, no new synchronization.
3. `tests/test_put_window_interface.c` links the real `put_window.c`
   through the header, with named `test_*` cases and the case filter of
   T7; one case per operation in the header.
4. Register the test (see `tests/README.md` if T8 has merged, else copy
   what `test_find_res_interface` has in the Makefile, `budgets.tsv` and
   `sanitizer_check.sh`).
5. On node9900: `make test`; `python3 scripts/tests_for.py client/write
   --run` (needs `make all` and the special binaries of `make test-long`
   first); ASan and TSan lanes for the new test.
6. "After" section: the same question measured again; update the map and
   the guide.

**Done when.** `nm` on the built `write.o` shows no `put_win` symbol
defined; the instruction count of the moved functions is unchanged with
inlining off (the method is on the `.find` ownership page); `make test`
rc 0, the routed tests rc 0, both sanitizer lanes PASS on node9900;
`python3 scripts/check_concern_map.py` passes with the moved functions
listed under the new file; your page has the before and after table.

**If something is unclear.** If a `put_win_*` function reads dirty-cache
state directly, do not widen the header to expose it: stop and report the
function and the field.
