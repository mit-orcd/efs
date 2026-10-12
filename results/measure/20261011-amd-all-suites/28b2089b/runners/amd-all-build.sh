#!/bin/bash
set -uo pipefail
src=/data1/efs/release-v030-28b2089b
out=/data1/efs/release-v030-results-20261012-28b2089b
mkdir -p "$out"
trap 'echo $? > "$out/build-and-gates.exit"' EXIT
cd "$src"
printf '%s\n' 28b2089b > "$out/source-commit.txt"
for mode in fault normal; do
 faults=0; [ "$mode" != fault ] || faults=1
 make clean > "$out/$mode-clean.log" 2>&1
 set +e
 make -j8 EFS_FAULTS="$faults" EFS_GIT_ID=28b2089b-release-$mode EFS_GIT_BRANCH=release-v0.3.0-pre-alpha EFS_GIT_DESCRIBE=v0.3.0-pre-alpha-candidate all test > "$out/$mode-native.log" 2>&1
 rc=$?
 set -e
 echo "$rc" > "$out/$mode-native.exit"
 [ "$rc" = 0 ] || echo "native gate failed: $mode" >> "$out/issues.log"
 mkdir -p "$out/$mode-binaries"
 cp efsd efs-fuse efs-mgmt "$out/$mode-binaries/"
 sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/$mode-binary-sha256.txt"
 ./efsd --version > "$out/$mode-version.txt"
done
python3 tests/test_client_memory.py --sanitize > "$out/client-memory-sanitizers.log" 2>&1
echo "$?" > "$out/client-memory-sanitizers.log.exit"
python3 tests/test_rollup_allocation_failure.py --sanitize > "$out/rollup-allocation-sanitizers.log" 2>&1
echo "$?" > "$out/rollup-allocation-sanitizers.log.exit"
python3 tests/test_rollup_budget.py --sanitize > "$out/rollup-budget-sanitizers.log" 2>&1
echo "$?" > "$out/rollup-budget-sanitizers.log.exit"
python3 tests/test_address_parser.py --sanitize > "$out/address-parser-sanitizers.log" 2>&1
echo "$?" > "$out/address-parser-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_publication_batch_rpc.py > "$out/publication-batch-sanitizers.log" 2>&1
echo "$?" > "$out/publication-batch-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_mounted_pack_batch.py > "$out/mounted-batch-sanitizers.log" 2>&1
echo "$?" > "$out/mounted-batch-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_packed_gc_budget.py > "$out/packed-gc-budget-sanitizers.log" 2>&1
echo "$?" > "$out/packed-gc-budget-sanitizers.log.exit"
WRITER_RPC_TEST_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_pack_authority_net_fallback.py > "$out/authority-net-sanitizers.log" 2>&1
echo "$?" > "$out/authority-net-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_capture_retry.py > "$out/query-capture-sanitizers.log" 2>&1
echo "$?" > "$out/query-capture-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_page_barriers.py > "$out/query-page-sanitizers.log" 2>&1
echo "$?" > "$out/query-page-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_fold_observation.py > "$out/fold-observation-sanitizers.log" 2>&1
echo "$?" > "$out/fold-observation-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_read_retry_budget.py > "$out/read-retry-sanitizers.log" 2>&1
echo "$?" > "$out/read-retry-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_host_read_deadline.py > "$out/read-deadline-sanitizers.log" 2>&1
echo "$?" > "$out/read-deadline-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_peer_pool_deadline.py > "$out/peer-pool-sanitizers.log" 2>&1
echo "$?" > "$out/peer-pool-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_readindex_remote_pair.py > "$out/remote-pair-sanitizers.log" 2>&1
echo "$?" > "$out/remote-pair-sanitizers.log.exit"
EFS_TEST_SANITIZE=1 python3 tests/test_append_cohort.py > "$out/append-cohort-sanitizers.log" 2>&1
echo "$?" > "$out/append-cohort-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_root_refusals.py > "$out/query-root-sanitizers.log" 2>&1
echo "$?" > "$out/query-root-sanitizers.log.exit"
# Separate complete sanitized library; retain normal mounted binaries unchanged.
san="$out/real-lsm-sanitizer-src"
mkdir -p "$san"
tar --exclude=.git --exclude=results --exclude="*.o" --exclude="*.a" -cf - Makefile include src deps tests | tar -xf - -C "$san"
make -C "$san" -B -j8 tests/test_query_catalog tests/test_retained_namespace_audit CFLAGS="-O1 -g -std=c99 -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined -lpthread -lm -ldl -libverbs" > "$out/real-lsm-sanitizer-build.log" 2>&1
echo "$?" > "$out/real-lsm-sanitizer-build.log.exit"
ASAN_OPTIONS=detect_leaks=1 "$san/tests/test_query_catalog" > "$out/real-lsm-sanitizer.log" 2>&1
echo "$?" > "$out/real-lsm-sanitizer.log.exit"
ASAN_OPTIONS=detect_leaks=1 "$san/tests/test_retained_namespace_audit" > "$out/real-lsm-audit-sanitizer.log" 2>&1
echo "$?" > "$out/real-lsm-audit-sanitizer.log.exit"
sha256sum -c "$out/normal-binary-sha256.txt" > "$out/normal-binary-retained.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_namespace_envelope_fixture.py > "$out/namespace-envelope-sanitizers.log" 2>&1
echo "$?" > "$out/namespace-envelope-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_work_trace.py > "$out/query-work-sanitizers.log" 2>&1
echo "$?" > "$out/query-work-sanitizers.log.exit"
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_retained_orphan.py > "$out/retained-orphan-sanitizers.log" 2>&1
echo "$?" > "$out/retained-orphan-sanitizers.log.exit"
python3 tests/test_posix_phase_trace.py > "$out/phase-observer-gate.log" 2>&1
echo "$?" > "$out/phase-observer-gate.log.exit"
echo completed > "$out/sanitizers.finished"
