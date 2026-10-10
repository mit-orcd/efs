# Intel large-directory collection follow-up — October 10, 2026

The tiny-file resume on Intel1 spent over twenty minutes enumerating its
preserved tree before writing. That observation alone does not isolate a
single cause. Two independently verified costs were found: explicit attributes
for each root folder in the generator, and quadratic name deduplication in the
FUSE directory-page collector. The generator fix avoids only redundant folder
attributes; every existing file still receives its size/type validation.

The FUSE collector compared each incoming name with every prior entry across
all shard pages. A fixture executing the unchanged production function makes
16,777,216 comparisons for 4096 unique names and their duplicate shard pages.
The repaired function uses a temporary open-addressed index of entry positions,
keeps first-seen inode/mode values, and makes 7148 comparisons for that fixture.
The index survives entry-array reallocations and is freed after the listing is
filled, including listing errors. This does not introduce paged FUSE handles:
the existing per-open handle still materializes the complete directory listing.

The production-function regression checks deterministic hash collisions,
zero-inode/empty/hidden names, duplicate preservation, empty pages, allocation
failures before/after populated pages, retry after failure, and size overflow.
It passes locally both normally and under ASan/UBSan. The original source fails
the comparison bound, establishing the specific improvement independently of
wall-clock timing. Intel live and full-suite acceptance are recorded after
running this source; no end-to-end throughput claim is inferred from the fixture.

A larger macOS execution of the actual collector uses 65536 unique names plus
their duplicate pages. The original makes 4,294,967,296 comparisons and the
indexed version 129122. The fixture execution wall times were 10.075 s and
0.313 s, excluding compilation but including Python process-wait overhead.
This is a local collector fixture, not a measured Intel creation-rate gain.
The source fixture also passes on both Intel Linux hosts.
