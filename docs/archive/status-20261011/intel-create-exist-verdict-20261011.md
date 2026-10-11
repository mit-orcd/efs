# Authoritative CREATE EXIST verdict — 2026-10-11

The FUSE creation errno helper received a proven EFS_ERR_EXIST refusal, then performed a second LOOKUP. BUSY at that later lookup replaced the refusal with EBUSY; absence after a concurrent unlink/rename replaced it with EIO. The original refusal belongs to the request's own linearization point and remains valid even when the name or read authority subsequently changes. Retained negative-CREATE replay already preserves that contract on the server.

The helper now returns EEXIST directly for EFS_ERR_EXIST and issues no second lookup. It does not turn unknown IO/BUSY outcomes into existence, adopt competing inodes, or change any server replay/mutation rule. Quota and other genuine statuses retain their mappings.

The production helper test now checks unavailable/absent subsequent state cannot replace EXIST and no lookup is issued. It fails on 297ab13f and passes with the fix on macOS and both Intel Linux hosts; competing-MKDIR refusal/credential fixtures also pass. A separate Intel2 private peer-MKDIR race recorded EIO on 297ab13f. Its exact underlying cause was not captured, so that live failure remains evidence pending fresh deployed acceptance rather than being attributed conclusively to this helper.

## Historical scope

This dated repair record is archived after the earlier Intel deployment verified the corrected exclusive-MKDIR and source-cursor gates. The [775d935c acceptance](intel-deployment-acceptance-775d935c-20261011.md) retains the deployed results. Any original pending language above belongs to that snapshot. Final-candidate full suites, rollout and sustained tiny-file performance remain active in the [current Intel ledger](../../status/intel-deployment-acceptance-20261011.md); W97/W100/W101 are not closed by moving this note.
