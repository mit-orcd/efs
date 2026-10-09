# Legacy lease ownership guards — AMD direct/TCP

All OPEN protocols now respect prepared inode and exact close-obligation keys.
Legacy OPEN/CLOSE also respect prepared lease keys. Legacy session apply preserves
BUSY/business verdicts while permitting later RESOLVE entries to run; IO/NOMEM
still retain replicated retry. This prevents a successful legacy descriptor from
bypassing a cleanup transaction or bypassing an atomic pending lease image.

Previous session source fails the actual prepared-close legacy OPEN assertion.
Full AMD Linux tests and final prepared-lease/append units pass. Private live
fixture gc-direct-uzpctqyk, port29440, pressure off: actual legacy HOLD returns
BUSY behind a prepared absent close marker; after ABORT/RESOLVE, OPEN/CLOSE
succeed. Deferred close and session append64 cold gates pass, core212/212,
peer64/64, append/fold audit and cold physical GC pass. Both installed mounts
restored. The helper's misleading-indentation warning was corrected afterward;
final identical helper logic compiles with -Wall -Wextra -Werror.

Frozen daemon SHA256 6d9c4a7e302b580153e42b1e4d18a812e75a0feb70c91cce62fab9ea5710ddc2

This is an ownership guard prerequisite, not production sequenced lease/session
activation. Legacy FUSE still has unresolved release retention/session lifecycle
work. No pressure/D25 activation or hardware durability claim.
