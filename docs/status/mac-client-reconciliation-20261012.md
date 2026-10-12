# Mac CREATE client reconciliation

Compared frozen Mac implementation e798c2aa (handoff 51b67e96) with release d26e6874. No release deployment or shared release edits were made.

## Conclusion

No missing combined CREATE/OPEN feature was found in the release client. The earlier claim that release needed the client portion of f842cd37 was too broad: release has an independent combined-adoption implementation from b9b55cba, with failure cleanup in 718e2dbc and reply-loss/cold gate work in 05026d42. Its current production code and focused tests preserve that implementation. Do not transplant f842cd37 or the adopted-field fixture from 51215bb4 over it merely to align struct layouts. Release d26e6874 correctly repairs the fixture to match its actual production source.

## Ownership and failure contracts

Mac f842cd37 keeps the original pending CREATE object referenced by the normal OPEN owner until exact combined OPEN/CLEAR_PENDING proof, then sets adopted and suppresses duplicate CLEAR_PENDING. Unknown replies keep the object queued until the edge settles.

Release d26e6874 instead transfers the exact generation/token/sequence and combined flags into owner->adoption before issuing the combined request, sets handed_off only after lease identity allocation, and retains that owner across ambiguous replies. The original completed CREATE object can then be freed without losing the retained token body. Cleanup resolves adoption before normal CLOSE. Existing local openers use a separate retained CLEAR_PENDING edge. A known negative combined verdict establishes no lease and must not invent CLOSE. A negative verdict learned after an unknown reply preserves the token while admission for a distinct pending-only CLEAR fails; it uses the CREATE generation rather than the stale rejected generation. The release regression explicitly tests these branches, competing OPEN serialization, inherited deadlines and invalid identities.

These are alternative client bookkeeping designs using the same existing lease wire contract, not a required protocol migration. Keep the independently integrated release implementation. The Mac pointer/adopted arrangement is optional reconciliation work and should be held; no production patch is warranted by the observed fixture mismatch.

## Fresh validation

Archived exactly d26e6874 include/, src/client/ and the focused tests into an isolated local directory. Ran test_fuse_lease_retention.py and test_pending_create_cleanup.py on both frozen Mac source and the exact release archive, normally and with AddressSanitizer/UndefinedBehaviorSanitizer: all eight executions PASS, exit 0. Tests execute extracted production bookkeeping and exercise ambiguity, immutable identities and cleanup; this is not a new live-cluster acceptance claim.

Raw results: evidence/mac-client-reconciliation-d26e6874.txt. Prior Mac deployed e798c2aa native POSIX remains 216/216 PASS; no cluster mode or binary was changed for this reconciliation. Broader coherent-cut/query starvation remains open and is outside this bounded assignment. No new wire/durable IDs were reserved.
