.DEFAULT_GOAL := all

CC = gcc
# -g keeps symbols for `perf report`; -fno-omit-frame-pointer improves stack
# unwinding under --perf. Both are cheap at -O3.
# Always tune for the build host (compile on the same arch you run on).
# Build id: nodes refuse to cluster across different builds (see HELLO gate).
EFS_GIT_ID := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
ifneq ($(shell git status --porcelain 2>/dev/null | head -1),)
EFS_GIT_ID := $(EFS_GIT_ID)-dirty
endif
# Human-facing version string (efs/version.h --version, startup logs, the
# VERSION wire op). Informational only; EFS_BUILD_ID above stays the gate.
EFS_GIT_DESCRIBE := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
EFS_GIT_BRANCH   := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
EFS_BUILD_TIME   := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
CFLAGS = -O3 -g -fno-omit-frame-pointer -march=native -mtune=native \
         -std=c99 -Wall -Wextra -D_GNU_SOURCE \
         -Wno-stringop-truncation -Wno-format-truncation \
         -DEFS_BUILD_ID='"$(EFS_GIT_ID)"' \
         -DEFS_VERSION='"$(EFS_GIT_DESCRIBE)"' \
         -DEFS_GIT_BRANCH='"$(EFS_GIT_BRANCH)"' \
         -DEFS_BUILD_TIME='"$(EFS_BUILD_TIME)"' $(EXTRA_DEFS)
ifeq ($(EFS_FAULTS),1)
CFLAGS += -DEFS_FAULTS=1
endif
INCLUDES = -Iinclude -Isrc/common -Ideps/blake3

LDFLAGS = -lpthread -lm -ldl -libverbs

COMMON_DIR = src/common
BLAKE3_DIR = deps/blake3

# ISA kernels. Each x86 file is compiled with its own -m flag so a Zen2
# -march=native build can still include the AVX-512 object. aarch64 uses
# the NEON kernel (baseline; no extra -m). Anywhere else, dispatch falls
# through to the portable kernel.
CC_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null || uname -m)
ifneq (,$(filter aarch64% arm64%,$(CC_TARGET)))
BLAKE3_ARCH_SRCS = $(BLAKE3_DIR)/blake3_neon.c
else ifneq (,$(filter x86_64% i386% i686% amd64%,$(CC_TARGET)))
BLAKE3_ARCH_SRCS = $(BLAKE3_DIR)/blake3_sse2.c \
              $(BLAKE3_DIR)/blake3_sse41.c \
              $(BLAKE3_DIR)/blake3_avx2.c \
              $(BLAKE3_DIR)/blake3_avx512.c
else
BLAKE3_ARCH_SRCS =
endif

COMMON_SRCS = $(COMMON_DIR)/publication.c $(COMMON_DIR)/publication_ack.c \
              $(COMMON_DIR)/common.c \
              $(COMMON_DIR)/log_ts.c \
              $(COMMON_DIR)/version.c \
              $(COMMON_DIR)/protocol.c \
              src/wire/wire.c \
              src/data/erasure.c \
              src/data/store_mem.c \
              src/data/transport_loop.c \
              src/data/transport_conn.c \
              src/kv/kv_mem.c \
              src/kv/kv_key.c \
              src/kv/kv_lsm.c \
              src/kv/kv_compact.c \
              src/kv/kv_wal.c \
              src/kv/kv_seg.c \
              src/kv/kv_snap.c \
              src/meta/metadata.c \
              src/meta/meta_apply.c \
              src/meta/txn.c \
              src/meta/session.c \
              src/meta/lock.c \
              src/meta/dir_layout.c \
              src/meta/dir_spread.c \
              src/sim/opid.c \
              src/raft/raft.c \
              src/raft/raft_mem.c \
              src/raft/raft_disk.c \
              src/raft/raft_log.c \
              $(COMMON_DIR)/placement.c \
              $(COMMON_DIR)/checksum.c \
              $(COMMON_DIR)/network.c \
              $(COMMON_DIR)/rdma.c \
              $(BLAKE3_DIR)/blake3.c \
              $(BLAKE3_DIR)/blake3_portable.c \
              $(BLAKE3_DIR)/blake3_dispatch.c \
              $(BLAKE3_ARCH_SRCS) \
              src/client/client.c \
              src/client/ops.c \
              src/client/read.c \
              src/client/write.c \
              src/client/bufpool.c src/client/writer_state.c src/client/publication_ack_owner.c \
              src/client/inode_rpc.c \
              src/client/stage_evict.c \
              src/client/node_cache.c

COMMON_OBJS = $(COMMON_SRCS:.c=.o)
LIB = libefs.a

TEST_SRCS = tests/test_store_overwrite.c tests/test_writer_routing.c tests/test_kv_seg_index.c tests/test_mtime_barrier.c tests/test_publication_session.c tests/test_publication_ack.c tests/test_publication_recovery.c tests/test_lane_bootstrap_recovery.c tests/test_fence_view.c tests/test_reply_buffers.c tests/test_bufpool.c tests/test_erasure.c tests/test_placement.c tests/test_rdma_xprt.c tests/test_wire.c tests/test_data.c tests/test_kv.c tests/test_kv_lsm.c tests/test_raft_store.c tests/test_meta_apply.c tests/test_raft.c tests/test_sim.c tests/test_txn.c tests/test_session.c tests/test_lock.c tests/test_stage_evict.c tests/test_conn_fd.c
TEST_BINS = tests/test_store_overwrite tests/test_writer_routing tests/test_kv_seg_index tests/test_mtime_barrier tests/test_publication_session tests/test_publication_ack tests/test_publication_recovery tests/test_lane_bootstrap_recovery tests/test_fence_view tests/test_reply_buffers tests/test_bufpool tests/test_erasure tests/test_placement tests/test_rdma_xprt tests/test_wire tests/test_data tests/test_kv tests/test_kv_lsm tests/test_raft_store tests/test_meta_apply tests/test_raft tests/test_sim tests/test_txn tests/test_session tests/test_lock tests/test_stage_evict tests/test_conn_fd

SERVER_SRCS = src/server/efsd.c src/server/store.c src/server/store_nvme.c \
              src/server/handler.c \
              src/server/cluster.c \
              src/server/peer_pool.c src/server/writer.c \
              src/server/iostats.c src/server/thread.c \
              src/server/raft_host.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = src/client/efs_fuse.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

BENCH_CLIENT_SRC = src/client/efs_bench.c src/bench/bench_local.c src/bench/blake3_bench.c src/bench/io_bench.c
# Reuse production storage/writer code without linking daemon/network startup.
# Separate sectioned objects let the linker discard unrelated server functions.
BENCH_STORE_OBJS = src/bench/store.o src/bench/store_nvme.o \
                   src/bench/writer.o src/bench/iostats.o src/bench/thread.o
src/bench/%.o: src/server/%.c .build_id.stamp
	$(CC) $(CFLAGS) $(INCLUDES) -DEFS_BENCH_BUILD -ffunction-sections -fdata-sections -c -o $@ $<

BENCH_CLIENT_OBJ = $(BENCH_CLIENT_SRC:.c=.o)
src/bench/bench_local.o src/bench/io_bench.o: src/bench/perf_control.h
$(BENCH_CLIENT_OBJ): src/bench/bench_local.h src/bench/blake3_bench.h src/bench/io_bench.h
$(BENCH_STORE_OBJS) src/bench/bench_local.o src/server/thread.o: src/server/server_internal.h

MGMT_SRC = src/mgmt/efs_mgmt.c
MGMT_OBJ = $(MGMT_SRC:.c=.o)

QUERY_SRC = src/query/efs_query.c
QUERY_OBJ = $(QUERY_SRC:.c=.o)

# System fuse3 (>= 3.12, bounded active-worker API). pkg-config supplies -I and -lfuse3.
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)

BLAKE3_OBJS = $(BLAKE3_DIR)/blake3.o \
              $(BLAKE3_DIR)/blake3_portable.o \
              $(BLAKE3_DIR)/blake3_dispatch.o \
              $(BLAKE3_ARCH_SRCS:.c=.o)

.PHONY: all clean tests test test-client-memory docs-check blake3-bench FORCE

.PHONY: test-fence-read test-fence-view test-lookup-memo test-fold-observation test-create-errors test-report-pressure
test-report-pressure:
	python3 tests/test_report_pressure.py
	python3 tests/test_write_queue.py
	python3 tests/test_report_orphan.py

test-create-errors:
	python3 tests/test_create_errors.py
	python3 tests/test_open_lease.py
	python3 tests/test_unlink_verdict.py

test-fold-observation:
	python3 tests/test_fold_observation.py
	python3 tests/test_load_merge_race.py
	python3 tests/test_writer_fence_merge.py
	python3 tests/test_writer_view_rpc.py
	python3 tests/test_lane_writer_view_rpc.py
	python3 tests/test_lane_writer_host.py
	python3 tests/test_lane_bootstrap_rpc.py
	python3 tests/test_lane_bootstrap_host.py
	python3 tests/test_writer_admission_rpc.py
	python3 tests/test_writer_deadline.py
	python3 tests/test_span_selection.py

test-lookup-memo:
	python3 tests/test_lookup_memo.py

tests/test_fence_view: tests/test_fence_view.c include/efs/fence_view.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $<

test-fence-read:
	python3 tests/test_fence_read.py

test-fence-view: tests/test_fence_view
	./tests/test_fence_view

.PHONY: test-dirty-ranges
test-dirty-ranges:
	@set -e; ranges_test=$$(mktemp /tmp/efs-test-dirty-ranges.XXXXXX); \
	trap 'rm -f "$$ranges_test"' EXIT; \
	$(CC) $(CFLAGS) $(INCLUDES) -o "$$ranges_test" tests/test_dirty_ranges.c; \
	"$$ranges_test"

.PHONY: test-writer-ranges
test-writer-ranges:
	@set -e; writer_test=$$(mktemp /tmp/efs-test-writer-ranges.XXXXXX); \
	trap 'rm -f "$$writer_test"' EXIT; \
	$(CC) $(CFLAGS) $(INCLUDES) -o "$$writer_test" tests/test_writer_ranges.c src/common/common.c; \
	"$$writer_test"

.PHONY: test-wb-recovery test-wb-runtime test-stop-control
test-stop-control:
	@set -e; stop_test=$$(mktemp /tmp/efs-test-stop-control.XXXXXX); \
	trap 'rm -f "$$stop_test"' EXIT; \
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/client -pthread -o "$$stop_test" tests/test_stop_control.c; \
	"$$stop_test"
	python3 tests/test_client_stop.py
	python3 tests/test_client_ready.py
	python3 tests/test_client_processes.py

test-wb-runtime:
	python3 tests/test_wb_runtime.py
	python3 tests/test_wb_fault.py
	python3 tests/test_stale_recovery.py
	python3 tests/test_pull_deadline.py

test-wb-recovery:
	@set -e; wb_test=$$(mktemp /tmp/efs-test-wb-recovery.XXXXXX); \
	trap 'rm -f "$$wb_test"' EXIT; \
	$(CC) $(CFLAGS) $(INCLUDES) -o "$$wb_test" tests/test_wb_recovery.c; \
	"$$wb_test"

.PHONY: test-client-stop
test-client-stop:
	python3 tests/test_client_stop.py
	python3 tests/test_client_ready.py
	python3 tests/test_client_processes.py

all: $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query tests

# blake3-bench always relinks so a stale binary cannot linger after CPU changes.
FORCE:

# Architecture doc machine-gate (docs/how-it-works/developing.md). Python only;
# safe on the login node. `make test` on a build node runs it too.
docs-check:
	python3 docs/check-architecture.py

test: all
	python3 tests/test_mgmt_session_routing.py
	python3 tests/test_session_admission.py
	python3 tests/test_publication_host.py
	python3 tests/test_publication_rpc.py
	python3 tests/test_bench_cli.py
	python3 tests/test_bench_profile.py
	python3 docs/check-architecture.py
	$(MAKE) test-fence-read
	$(MAKE) test-fence-view
	$(MAKE) test-dirty-ranges
	$(MAKE) test-writer-ranges
	$(MAKE) test-wb-recovery
	$(MAKE) test-wb-runtime
	$(MAKE) test-stop-control
	$(MAKE) test-lookup-memo
	$(MAKE) test-fold-observation
	$(MAKE) test-create-errors
	$(MAKE) test-report-pressure
	./tests/test_reply_buffers
	./tests/test_reply_buffers key-failure
	./tests/test_bufpool
	python3 tests/test_read_admission.py
	python3 tests/test_client_memory.py
	./tests/test_wire
	./tests/test_data
	./tests/test_kv
	./tests/test_kv_lsm
	./tests/test_store_overwrite
	./tests/test_writer_routing
	./tests/test_kv_seg_index
	./tests/test_raft_store
	./tests/test_mtime_barrier
	./tests/test_publication_session
	./tests/test_publication_ack
	./tests/test_publication_recovery
	./tests/test_meta_apply
	./tests/test_lane_bootstrap_recovery
	./tests/test_raft
	./tests/test_sim
	./tests/test_txn
	./tests/test_session
	./tests/test_erasure
	./tests/test_placement
	./tests/test_lock
	./tests/test_stage_evict
	./tests/test_conn_fd

# Rebuild when public headers change (struct layouts in metadata.h, etc.).
# .build_id.stamp changes content (and mtime) only when the git id changes,
# so every object picks up a moved HEAD or dirty-flag flip — a stale object
# keeping the old -DEFS_BUILD_ID splits the cluster at the HELLO gate.
.build_id.stamp: FORCE
	@echo '$(EFS_GIT_ID)' | cmp -s - $@ 2>/dev/null || echo '$(EFS_GIT_ID)' > $@

$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(MGMT_OBJ) $(QUERY_OBJ): \
	include/efs/common.h include/efs/metadata.h include/efs/protocol.h \
	include/efs/wire.h include/efs/store.h include/efs/transport.h \
	include/efs/kv.h include/efs/fence_view.h \
	.build_id.stamp

src/wire/wire.o: include/efs/raft.h

$(LIB): $(COMMON_OBJS)
	ar rcs $@ $^

efsd: $(SERVER_OBJS) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SERVER_OBJS) $(LIB) $(LDFLAGS)

efs-fuse: $(CLIENT_OBJS) $(LIB)
	@pkg-config --atleast-version=3.12 fuse3 || { \
	  echo "efs-fuse needs fuse3-devel >= 3.12 (pkg-config fuse3)" >&2; \
	  exit 1; }
	$(CC) $(CFLAGS) $(INCLUDES) $(FUSE_CFLAGS) -o $@ $(CLIENT_OBJS) $(LIB) $(LDFLAGS) $(FUSE_LIBS)

efs-bench: $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--gc-sections -o $@ $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(LIB) $(LDFLAGS)

$(CLIENT_OBJS): CFLAGS += $(FUSE_CFLAGS)

efs-mgmt: $(MGMT_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(MGMT_OBJ) $(LIB) $(LDFLAGS)

efs-query: $(QUERY_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(QUERY_OBJ) $(LIB) $(LDFLAGS)

# These exercise production ownership/budget helpers with allocator/RDMA stubs.
tests/test_reply_buffers: tests/test_reply_buffers.c src/client/reply_buffers.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< -lpthread

tests/test_bufpool: tests/test_bufpool.c src/common/common.c src/client/publication_ack_owner.c include/efs/publication_ack.h src/client/bufpool.c src/client/writer_state.c include/efs/writer_state.h include/efs/writer_ranges.h src/client/client_internal.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< src/common/common.c -lpthread

test-client-memory: tests/test_reply_buffers tests/test_bufpool
	python3 tests/test_fuse_workers.py
	./tests/test_reply_buffers
	./tests/test_reply_buffers key-failure
	./tests/test_bufpool
	python3 tests/test_read_admission.py
	python3 tests/test_client_memory.py

$(CLIENT_OBJS) $(BENCH_CLIENT_OBJ): src/client/client_internal.h include/efs/wb_recovery.h
src/client/efs_fuse.o: src/client/reply_buffers.h src/client/stop_control.h

tests/test_store_overwrite: tests/test_store_overwrite.c src/server/store.c src/server/server_internal.h src/bench/writer.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o $(LIB) $(LDFLAGS)

tests/test_writer_routing: tests/test_writer_routing.c src/server/writer.c src/server/server_internal.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< -lpthread

tests: $(TEST_BINS)

src/sim/sim.o: src/sim/sim.c src/sim/sim_internal.h include/efs/sim.h include/efs/store.h include/efs/kv.h include/efs/meta_apply.h include/efs/transport.h include/efs/opid.h include/efs/raft.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_raft.o: src/sim/sim_raft.c src/sim/sim_internal.h include/efs/sim.h include/efs/raft.h include/efs/meta_apply.h include/efs/opid.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_ctrl.o: src/sim/sim_ctrl.c src/sim/sim_internal.h include/efs/sim.h include/efs/raft.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_txn.o: src/sim/sim_txn.c src/sim/sim_internal.h include/efs/sim.h include/efs/txn.h include/efs/kv_key.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_sess.o: src/sim/sim_sess.c src/sim/sim_internal.h include/efs/sim.h include/efs/session.h include/efs/kv_key.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_dir.o: src/sim/sim_dir.c src/sim/sim_internal.h include/efs/sim.h include/efs/dir_layout.h include/efs/kv_key.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_lock.o: src/sim/sim_lock.c src/sim/sim_internal.h include/efs/sim.h include/efs/lock.h include/efs/kv_key.h include/efs/meta_cmd.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_disk.o: src/sim/sim_disk.c src/sim/sim_internal.h include/efs/sim.h include/efs/kv.h include/efs/kv_lsm.h include/efs/raft_disk.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

src/sim/sim_ns.o: src/sim/sim_ns.c src/sim/sim_internal.h include/efs/sim.h include/efs/txn.h include/efs/kv_key.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

tests/test_sim: tests/test_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_ns.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/sim_disk.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ tests/test_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_ns.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/sim_disk.o $(LIB) $(LDFLAGS)

# Not part of `make test`: the suite exits 2 while repair is a gap, and
# that must not be turned into a green unit run.
tests/faults/fault_sim: tests/faults/fault_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_ns.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/sim_disk.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ tests/faults/fault_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_ns.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/sim_disk.o $(LIB) $(LDFLAGS)

%: %.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

blake3-bench: $(BLAKE3_OBJS) src/bench/blake3_bench.o FORCE
	$(CC) $(CFLAGS) $(INCLUDES) \
		-o blake3-bench tools/blake3-bench.c src/bench/blake3_bench.o $(BLAKE3_OBJS) $(LDFLAGS)

clean:
	rm -f $(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(MGMT_OBJ) $(QUERY_OBJ)
	rm -f src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_ns.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/sim_disk.o src/sim/opid.o
	rm -f $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query blake3-bench
	rm -f tests/perf/tcp_rdma/xprt_bench tests/faults/fault_sim
	rm -f $(TEST_BINS)
	rm -f .build_id.stamp

$(BLAKE3_DIR)/blake3_sse2.o: $(BLAKE3_DIR)/blake3_sse2.c
	$(CC) $(CFLAGS) $(INCLUDES) -msse2 -c -o $@ $<

$(BLAKE3_DIR)/blake3_sse41.o: $(BLAKE3_DIR)/blake3_sse41.c
	$(CC) $(CFLAGS) $(INCLUDES) -msse4.1 -c -o $@ $<

$(BLAKE3_DIR)/blake3_avx2.o: $(BLAKE3_DIR)/blake3_avx2.c
	$(CC) $(CFLAGS) $(INCLUDES) -mavx2 -maes -c -o $@ $<

$(BLAKE3_DIR)/blake3_avx512.o: $(BLAKE3_DIR)/blake3_avx512.c
	$(CC) $(CFLAGS) $(INCLUDES) -mavx512f -mavx512vl -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
