# Development — modularity as an architectural constraint

[Architecture](../architecture.md) · [Start here](START-HERE.md) ·
[Verification](verification.md) · [Scaling roadmap](../scaling-roadmap.md)

> Looking for something to do rather than a principle to follow?
> [START-HERE.md](START-HERE.md) turns this page's bar into a task list: the
> current task, which pages govern a given change, and what "done" means.

Modularity is not a style preference here — it is what makes the two things
this project depends on possible at all: **fast isolated testing** (see
[verification.md](verification.md)) and **bounded-context change** (a human
or a model editing one component without ingesting the whole codebase). A
system this subtle cannot afford either to be slow to test or to require
global knowledge to change safely.

**The bar, stated plainly: a *less advanced* AI model must be able to
contribute a correct change.** Not "the best available model, with the whole
tree in context, on a good day" — a modest one. That is a much stronger
requirement than "the code is organized," and it has concrete consequences:

- **Local correctness must be locally decidable.** Whether a change to module
  X is right must be answerable from X's source + X's interface header + X's
  tests — never from global reasoning about the whole system. If correctness
  requires holding the whole tree in your head, only the strongest
  contributors can play, and the project scales with *model quality* instead
  of with *contributor count*.
- **The blast radius of a mistake is one module.** A weak contributor's error
  must fail *fast and locally* — a unit test, a simulator assertion, an
  interface-contract check — not subtly, three subsystems away, in
  production. The safety net is executable (tests, invariants, the
  simulator), so the quality bar is enforced by the *boundaries*, not by the
  sophistication of whoever is editing.
- **Interfaces carry the contract.** Each module's header states what it
  guarantees and what it requires (invariants, ownership, threading rules) —
  so a contributor doesn't have to *derive* the contract from the rest of the
  tree before touching anything.

**The rule.** A module must be understandable, changeable, and testable from
**its own source plus its interface header alone**. If you have to read the
rest of the tree to change one component safely, the modularity has failed —
regardless of how the directories are named.

**Where we are today (honest).** The code is *not* there. Four files hold
~45% of the 36.5k-line tree — `metadata.c` (6042 lines), `efs_fuse.c` (3720),
`meta_server.c` (3654), `handler.c` (3648). That is why every change is slow,
every test pulls in the world, and every review needs the whole file in
context. The target module boundaries below are drawn to fix exactly this.

**Target boundaries** (aligned with the planes, so the architecture and the
code structure are the same map):

```text
raft/       the consensus core — a pure state machine, transport- and
            storage-agnostic; no I/O inline, no globals. Testable in the
            simulator and in a unit harness alike.
kv/         the ordered applied state — behind a storage interface
            (real NVMe engine / simulated fault-injecting disk).
meta/       the POSIX op handlers — pure-ish functions over the kv/ and
            raft/ interfaces; no socket or FUSE calls inline.
wire/       the protocol — versioned encode/decode, nothing else.
data/       the data plane — EC encode/decode, RDMA PUT/GET, generation
            fencing.
client/     the FUSE adapter — thin; translates FUSE ops to meta/data calls.
```

**Rules that enforce it:**

- **Depend on the interface, not the implementation.** Modules include each
  other's *headers*, never reach into another module's `.c` internals. The
  current `g_server->lock` / shared-global pattern is the anti-example — it
  is what forced whole-subsystem context for every change.
- **State machines are pure.** No hidden globals, no I/O inline; all I/O goes
  through the transport/storage interfaces. This is *also* the property that
  lets the same compiled state machine run under the simulator
  ([verification.md](verification.md)) — purity buys testability and
  simulatability at once.
- **Every module has a unit test that links only its real dependencies** (or
  interface fakes) and runs in milliseconds. A change to `raft/` must not
  require a `client/` rebuild — mentally or literally.
- **Context budget.** A module plus its interface should fit in a few hundred
  lines, so a change can be made and reviewed with bounded context. Working
  target: **no source file over ~1000 lines**; split by responsibility when
  one crosses it. This is the property that lets a model (or a new
  contributor) load one module and make a correct change without the whole
  tree in scope.
- **Conventions are machine-checkable.** Formatting, naming, and the
  interface/ownership rules are enforced by tooling (lint, the build, the
  test gate), not by reviewer vigilance — a less advanced contributor
  (human or model) cannot silently violate a rule the tooling would have
  caught.

## The specification itself is under a machine gate

The same argument applies to the documents. A normative index plus satellites
is only safer than a monolith if the split cannot silently drift — otherwise
a contradiction between the index and a satellite is just a monolith with
extra steps, and the contributor who reads only one of them is misled. The
documentation gate is therefore part of the build, not an editorial habit:

```text
regenerate docs/architecture-full.md and fail on any diff
    -> the generated review artifact can never be stale

validate every internal link resolves
    -> the index-plus-satellite structure stays navigable

validate every invariant reference (I1..I25) names a defined invariant
    -> "preserves I21" cannot survive renumbering

validate every operation in the §6 matrix names an existing protocol section

reject a normative table defined in more than one place
    -> placement formulas, the op matrix and the invariant list have exactly
       one home; satellites explain them, never restate them
```

The last check is the one that matters most: duplicated normative tables are
how the index and a satellite come to disagree, and the rule "if they
disagree, the index wins" is a fallback, not a substitute for the tables
being single-sourced.

**Why it is in the architecture and not a style guide:** the migration
(§10 of the spec) lands Raft, KV, and the transaction protocol as *new*
components. If they are built to these boundaries from the start, the
simulator and the unit harness get them for free, and the dev cycle stays
fast as the system grows. If they are built as another 6000-line
`metadata.c`, no amount of testing infrastructure will save the cycle time.
