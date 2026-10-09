# Devel combined branch gates — October 9 UTC

All active development branch tips available at the final boundary are merged
into devel, including W66 and agent2/3/4 committed work. Working branches remain.
No main merge, push or tag. Production/test baseline 7b4f6085; subsequent
677522ec is documentation/evidence only.

AMD full Linux regression passed. Direct/TCP and buffered/TCP each pass core
POSIX 212/212, two-client 64/64 and cold persistence 26/26, no failures/skips.
Architecture 5/5 after regenerated docs.

[Evidence and runner](../../results/measure/20261009-devel-integration/README.md).
AMD original installation restored direct/TCP; command features disabled.
W66 topology/scaling and earlier Mac VM timeout attribution remain open.
