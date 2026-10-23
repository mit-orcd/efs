# Fixed-capacity apply expansion audit

AMD private direct-I/O/TCP, pressure enabled in fixture only. Full Linux build
and regression pass; no build warnings. Core/peer POSIX, physical GC effects,
cold inventory/quota zero and late-PUT fences pass. Installed 978caaf7 service
restored with pressure off.

Fixture: /data1/efs/evidence/w66-gc-fixtures/gc-direct-ohu4urqa

These functional gates do not certify universal memory bounds or pressure
activation. Session cleanup and remaining command coverage remain open.
