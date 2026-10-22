# mac-retained-orphan-policy.txt — evidence summary

## Purpose and linked item
One-line policy probe captured for the retained-orphan review, added by
commit `4124876fc` ("Reproduce retained Mac directory backlink refusal
with safe negative controls", 2026-10-11). Linked status page:
`docs/status/mac-retained-orphan-review-20261012.md`.

## Verdict
Informational capture — not a pass/fail run. The probe returned rc=0,
recording `commands-policy version=4 mask=3 leader_hint=1` at
2026-10-12 01:28:57 UTC.

## Source commit and build identity
Not recorded in this file; taken against the same deployment documented
in `mac-retained-orphan-runtime.txt` (`efsd v0.2.2-pre-alpha-55-ge798c2aa`
on efs1).

## Environment
Live single-node deployment efs1 (192.168.2.11:17432, `--no-direct-io`,
quota 10G per the sibling runtime capture). Timestamp: Mon Oct 12
01:28:57 UTC 2026.

## Method
Issue the commands-policy query against the running daemon and record
the reply and wall-clock time.

## Results
| Field | Value |
|---|---|
| rc | 0 |
| version | 4 |
| mask | 3 |
| leader_hint | 1 |

## Limitations and remaining gates
Single sample; no comparison baseline. Only meaningful together with
the sibling retained-orphan captures.

## Evidence index
- `mac-retained-orphan-policy.txt` sha256:c4267ea3c03aa49d — policy probe (this file)
- `mac-retained-orphan-runtime.txt` sha256:9dab03ca20b06ae8 — daemon identity for the same deployment
- `mac-retained-orphan-query-log.txt` sha256:8b060b74dbea3f9b — refusal log from the same window
