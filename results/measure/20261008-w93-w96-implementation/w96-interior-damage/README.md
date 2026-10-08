# Detectable interior WAL damage

An invalid frame followed by nonzero records must not be truncated as an EOF
tail. The recovery guard preserves the file and returns PROTO, including an
older-build zero gap before a later acknowledged record. Ordinary torn tails
and all-zero reservation tails still repair. This is not universal bit-rot
detection or a salvage tool: it does not claim to reconstruct corrupt records.

Production WAL fault tests cover bad CRC followed by acknowledged B and an
invalid zero header followed by B; neither permits truncation. Focused AMD KV
(compaction/reset/reopen), Raft and disk-store tests pass. Linux and Mac
ASan/UBSan fault suites pass. Full live cold gate rerun for this final guard
remains pending while isolated mixed-load comparisons occupy AMD.
