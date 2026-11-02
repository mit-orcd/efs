# Complete and bounded address-port parsing

October 11, 2026. S7 correctness checkpoint on `devel-code-review`. Only CLI/bootstrap address parsing changes; no public/native/wire API or network protocol change.

## Reproduced failure

Five old private parsers (`efs_fuse`, node cache, management, retired query CLI and daemon join) used `atoi` then narrowed to uint16: `host:65536` returned success with port 0, `host:-1` returned success with port 65535, and `host:12junk` returned success with port 12. The benchmark parser already rejected negative/out-of-range values, but also accepted `host:12junk`. An exact-function executable probe reproduced each result. The new regression against pre-fix `3ac828cd` fails its malformed-address rejection assertion.

## Change and compatibility

A small internal static helper validates nonempty host and port, host-buffer bounds, ASCII decimal digits with complete consumption, and range 1–65535 before assigning either output. Accumulation checks before multiply/add, so even long digit strings cannot overflow. All six private wrappers use the helper; their names/callers and public signatures remain unchanged.

Valid IPv4/hostname inputs and leading-zero decimal ports retain their values. Last-colon host splitting is unchanged, including bare IPv6-with-port and bracketed-host byte extraction. Host resolution/socket handling is deliberately outside this helper; these checks do not add bracketed IPv6 connection support or claim a new hostname grammar. Invalid signed, zero, overflowing, empty, whitespace-containing, partially numeric or hexadecimal ports now fail instead of silently selecting another endpoint. This intentional CLI rejection change does not affect persisted membership or wire fields. Other numeric CLI options are outside this patch.

All six original parser bodies were byte-identical to this review baseline at the inspected snapshots:

- `devel-orcd`: `7e249d070c9abd22279d8e94cf03274ba69088d2`
- `devel-nuc`: `a323e1271b11989808ab0da0cb7cba6edae82aa4`
- `devel-mac`: `b2c01fa05afa1287cd358f352ebfbc831b570e31`

## Focused gates

`tests/test_address_parser.py` extracts all six actual wrappers and includes the shared implementation. It tests normal endpoints, 1/65535, leading zeros, preserved IPv6 host splitting, invalid/range/long-digit cases, exact and short host buffers, unchanged outputs on failure and null helper arguments. It also exercises every valid numeric port from 1 through 65535. Ordinary and ASan/UBSan runs pass. The regression is wired into `make test` and `make test-client-memory`.

Complete node-cache, client benchmark, management, query CLI and daemon syntax checks pass locally. FUSE validation is its extracted actual wrapper, not a full Linux FUSE build. Documentation and whitespace gates pass. Real CLI exits, valid-endpoint discovery/join/membership and full merged Linux/live acceptance remain release-coordinator gates. No networking is performed by the parser regression.
