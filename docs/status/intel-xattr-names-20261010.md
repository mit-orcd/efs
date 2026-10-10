# Intel empty xattr names — 2026-10-10

Linux rejects SET, GET, and REMOVE of `user.` with EINVAL. Intel EFS at fcacc64a accepted all three. The shared FUSE name validator now rejects the empty user namespace before an RPC; valid names retain their existing behavior.

The production-function fixture exercises all three callbacks, namespace and length boundaries, successful mutations, missing attributes, and balanced request/mutation cleanup. It fails against the previous validator and passes with this change. The existing POSIX opt_xattr case now checks Linux EINVAL and preservation of its valid attribute. On Intel1 the stronger case failed against the previous deployed build and passed against native /tmp. The runner self-test also passed. Full deployed checks are recorded separately after rollout.
