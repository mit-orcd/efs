#!/bin/bash
set -euo pipefail
src=/tmp/efs-orcd-unit-b239dc7de104
out=/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-b239dc7de104/full-prerequisites
runtime=/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-b239dc7de104/sanitizer-runtime
export PATH="$runtime/bin:$PATH"
export LD_LIBRARY_PATH="$runtime/usr/lib64:/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH=/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse/lib/pkgconfig:${PKG_CONFIG_PATH:-}
trap 'echo $? > "$out/sanitizers-resume.exit"' EXIT
cd "$src"
cp "$out/client-memory-sanitizers.log" "$out/client-memory-missing-runtime.log"
python3 tests/test_client_memory.py --sanitize > "$out/client-memory-sanitizers.log" 2>&1
python3 tests/test_rollup_allocation_failure.py --sanitize > "$out/rollup-allocation-sanitizers.log" 2>&1
python3 tests/test_rollup_budget.py --sanitize > "$out/rollup-budget-sanitizers.log" 2>&1
python3 tests/test_address_parser.py --sanitize > "$out/address-parser-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_publication_batch_rpc.py > "$out/publication-batch-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_mounted_pack_batch.py > "$out/mounted-batch-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_packed_gc_budget.py > "$out/packed-gc-budget-sanitizers.log" 2>&1
WRITER_RPC_TEST_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_pack_authority_net_fallback.py > "$out/authority-net-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_capture_retry.py > "$out/query-capture-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_page_barriers.py > "$out/query-page-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_fold_observation.py > "$out/fold-observation-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_read_retry_budget.py > "$out/read-retry-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_host_read_deadline.py > "$out/read-deadline-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_peer_pool_deadline.py > "$out/peer-pool-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_readindex_remote_pair.py > "$out/remote-pair-sanitizers.log" 2>&1
EFS_TEST_SANITIZE=1 python3 tests/test_append_cohort.py > "$out/append-cohort-sanitizers.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_query_root_refusals.py > "$out/query-root-sanitizers.log" 2>&1
# Separate complete sanitized library; retain normal mounted binaries unchanged.
san="$out/real-lsm-sanitizer-src"
mkdir -p "$san"
tar --exclude=.git --exclude=results --exclude="*.o" --exclude="*.a" -cf - Makefile include src deps tests | tar -xf - -C "$san"
make -C "$san" -B -j8 tests/test_query_catalog CFLAGS="-O1 -g -std=c99 -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined -lpthread -lm -ldl -libverbs" > "$out/real-lsm-sanitizer-build.log" 2>&1
ASAN_OPTIONS=detect_leaks=1 "$san/tests/test_query_catalog" > "$out/real-lsm-sanitizer.log" 2>&1
sha256sum -c "$out/normal-binary-sha256.txt" > "$out/normal-binary-retained.log" 2>&1
CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer" python3 tests/test_namespace_envelope_fixture.py > "$out/namespace-envelope-sanitizers.log" 2>&1
echo 0 > "$out/sanitizers.exit"
