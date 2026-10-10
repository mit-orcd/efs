# Cleanup harness correction

`gc-direct-675xpxa7` passed twenty concurrent append tests, full peer POSIX
64/64 and cold exact bytes. The newly added cleanup branch then raised
NameError because its shutil import was missing. The harness failure remains
a failure; its results do not prove successful physical cleanup. Added the
import and repeated the complete gate on a separate unique private fixture.
C candidate source and binaries were unchanged by the harness correction.
