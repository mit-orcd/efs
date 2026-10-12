# AMD admission diagnostic 4e373f49

Private c10a0678 foundation plus diagnostic-only admitted-round/term/want and fence traces; excluded from release code. Direct TCP, four private nodes, seven clients/jobs16, original 30/75/180-second budgets. No simultaneous build during the run. Production services untouched.

Exit1,251.576seconds: each client211PASS/2FAIL/1SKIP/7NOTRUN. Failures are virt_stats_readable30-second timeout and subsequent virt_find_query signal15. Peer67/67PASS; mounted packed/ordinary acknowledged cold hashesPASS. Global physical-packed-empty check failed180seconds.

That global reclamation verdict cannot establish fixture-specific GC failure: all seven POSIX logs explicitly retain their test trees after surviving workers, while the final oracle deletes only mounted-pack and scans all packs. Live retained files may legitimately retain packs. OriginalFAIL remains preserved; a fresh isolated64-file no-POSIX cold/reclaim run is required to distinguish this scope mismatch from GC failure. Query acceptance remainsFAIL.

No admitted/publication timeout or term-fence diagnostics emitted. Remote-read -13 records occur at startup/shutdown; they do not establish load-time admission failure or justify weakening publication authority. Diagnostic observations are not a quiet acceptance gate.

raw-manifest.json records every original raw byte hash; large logs use lossless .gz with decompression equality verified. Binaries and storage payloads excluded.
