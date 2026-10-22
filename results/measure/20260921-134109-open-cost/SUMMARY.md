# SUMMARY — results/measure/20260921-134109-open-cost

Open/read cost vs file size, cold (remount before each size) vs warm, showing
the GETCHUNKS-at-open cost shape.

## Purpose and linked item

- W8-era client read-path cost study (open cost scaling with chunk count).
  Added with the W8-era commits; sibling runs `20260921-133437-posix-isolated`,
  `20260921-162514-ior-hard-scaling`.

## Verdict

**PASS** (measurement completed): cold open scales with chunk count via
GETCHUNKS batching (≤64 records per reply); warm open is flat (~30 ms).

## Source commit and build identity

`b2184a5c7faf-dirty` (recorded in SUMMARY.txt).

## Environment

Writer fcstor007, reader fcstor008, cold remount before each size.

## Method

Write files of 128/1024/4096 MiB, remount, then time cold open + first 4 KiB
read; repeat warm. RPC profile counters (`rpcprof-*.txt`) count
getchunks/lookup/getattr per open. Raw: `open-*.txt`, `remount-*.txt`.

## Results

| size MiB | cold open ms | cold read4k ms | cold total ms | warm open ms | getchunks n | getchunks avg µs |
|---|---|---|---|---|---|---|
| 128 | 100.9 | 20.8 | 121.8 | 33.3 | 16 | 1730 |
| 1024 | 227.9 | 21.2 | 249.1 | 29.5 | 128 | 1598 |
| 4096 | 359.1 | 20.1 | 379.2 | 27.0 | 512 | 640 |

getchunks_n ≈ chunks/64 per open — the whole chunk map is fetched at open.

## Limitations and remaining gates

Single reader/writer pair; the open-time chunk-map fetch is the documented cost
driver for large files.

## Evidence index

- `SUMMARY.txt` — `shasum -a 256 SUMMARY.txt | cut -c1-16` in this dir
- `open-128.txt` / `open-1024.txt` / `open-4096.txt` — per-size runs
- `remount-*.txt`, `preflight.txt`
