# W112 full-overwrite replay review — 2026-10-11

Pinned production repair: `fdfbd9c4da070b5b32d9d2d66e5b9441eb607220`. Baseline source: `d9d39acc446c050f9a17cb15da9f8e7465edbbcd`. Release worktree was read only; no changes were made there.

Disposition: no additional concrete defect found in the seven-line repair. Independently reproduced the new production-function regression failing on baseline (`sent[i]=='N'`), then passing on the pinned repair normally and with ASan/UBSan. This disposition supports the narrow byte-retention repair, not closure of mounted durability or full release qualification.

The branch requires the canonical UNCOND whole-overwrite sentinel, a complete retained body, no dirty partial ranges and no span object length. Snapshotting occurs under the existing cache mutex while the replay retains the slot IO mutex. It copies exactly the allocated snapshot length. The earlier absent-row branch is unchanged. Partial ranges and span objects remain in the old merge branches.

The fetched committed generation now becomes the retry base; the fetched byte-backed observation is still passed to `dcache_put_now`. Thus the replacement body deliberately owns the whole image while the fresh base, exact span count/sequence and captured content fence remain authority for its conditional retry. No status is converted into a successful publication, no durable receipt/ACK is synthesized, and existing PUT errors, snapshot sequence and image-install checks remain unchanged. Source inspection confirmed that `dcache_put_now_budgeted` transfers the observed span identity into `putid_note`/the object record and retains the captured fence epoch and FRESH_OBJECT flags. It does not turn the retry back into unconditional authority.

Additional independent tests in `test_fold_boundaries.py` extend the pinned fixture:

- A full retained overwrite against a different committed generation 73 sends every retained byte, base 73, the exact fetched span list/sequence, and fence epoch 6.
- UNCOND state with a partial dirty range preserves fetched peer spans and overlays only that range.
- UNCOND state with a retained span object preserves fetched peer spans and overlays only that span.

All extended probes passed normally and under ASan/UBSan. Existing partial/peer-span, tombstone, chain-full conversion, PUT race and failure cases still passed. The harness extracts actual replay and PUT functions but doubles fragment/RPC/metadata boundaries; no real disk, Raft durability, concurrent writer or cold-mounted result was produced in this chat. The reported primary/peer/cold 512-byte corruption evidence is external coordination evidence, not a verification performed here.

Re-run against an archive of the pinned repair:

```sh
snapshot=$(mktemp -d)
git archive fdfbd9c4da070b5b32d9d2d66e5b9441eb607220 src include deps tests Makefile | tar -x -C "$snapshot"
EFS_REVIEW_SNAPSHOT="$snapshot" python3 docs/reviews/w112-overwrite-20261011/test_fold_boundaries.py
EFS_REVIEW_SNAPSHOT="$snapshot" FOLD_TEST_CFLAGS='-fsanitize=address,undefined' python3 docs/reviews/w112-overwrite-20261011/test_fold_boundaries.py
```

No additional production patch, deployment, push, cross-cluster change or optional refactor was performed. Native/fault and actual ordinary overwrite primary/peer/cold checks in both storage modes remain required for closure.
