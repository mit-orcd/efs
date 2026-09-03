CC = gcc
# -g keeps symbols for `perf report`; -fno-omit-frame-pointer improves stack
# unwinding under --perf. Both are cheap at -O3.
# Always tune for the build host (compile on the same arch you run on).
# Build id: nodes refuse to cluster across different builds (see HELLO gate).
EFS_GIT_ID := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
ifneq ($(shell git status --porcelain 2>/dev/null | head -1),)
EFS_GIT_ID := $(EFS_GIT_ID)-dirty
endif
CFLAGS = -O3 -g -fno-omit-frame-pointer -march=native -mtune=native \
         -std=c99 -Wall -Wextra -D_GNU_SOURCE \
         -Wno-stringop-truncation -Wno-format-truncation \
         -DEFS_BUILD_ID='"$(EFS_GIT_ID)"' $(EXTRA_DEFS)
INCLUDES = -Iinclude -Isrc/common -Ideps/blake3

LDFLAGS = -lpthread -lm -ldl -libverbs

COMMON_DIR = src/common
BLAKE3_DIR = deps/blake3

COMMON_SRCS = $(COMMON_DIR)/common.c \
              $(COMMON_DIR)/protocol.c \
              src/wire/wire.c \
              src/data/erasure.c \
              src/data/store_mem.c \
              src/data/transport_loop.c \
              src/data/transport_conn.c \
              src/kv/kv_mem.c \
              src/kv/kv_key.c \
              src/meta/metadata.c \
              src/meta/meta_apply.c \
              src/meta/txn.c \
              src/meta/session.c \
              src/meta/lock.c \
              src/meta/dir_layout.c \
              src/sim/opid.c \
              src/raft/raft.c \
              src/raft/raft_mem.c \
              $(COMMON_DIR)/placement.c \
              $(COMMON_DIR)/checksum.c \
              $(COMMON_DIR)/network.c \
              $(COMMON_DIR)/rdma.c \
              $(BLAKE3_DIR)/blake3.c \
              $(BLAKE3_DIR)/blake3_portable.c \
              $(BLAKE3_DIR)/blake3_dispatch.c \
              $(BLAKE3_DIR)/blake3_sse2.c \
              $(BLAKE3_DIR)/blake3_sse41.c \
              $(BLAKE3_DIR)/blake3_avx2.c \
              $(BLAKE3_DIR)/blake3_avx512.c \
              src/client/client.c \
              src/client/ops.c \
              src/client/read.c \
              src/client/write.c \
              src/client/bufpool.c \
              src/client/inode_rpc.c \
              src/client/node_cache.c

COMMON_OBJS = $(COMMON_SRCS:.c=.o)
LIB = libefs.a

TEST_SRCS = tests/test_erasure.c tests/test_placement.c tests/test_integration.c tests/test_quota.c tests/test_migrate.c tests/test_directio.c tests/test_rejoin.c tests/test_query.c tests/test_list_exports.c tests/test_dir_stats.c tests/test_ino_path.c tests/test_meta_slot.c tests/test_add_storage.c tests/test_meta_cap.c tests/test_meta_v6.c tests/test_rdma_xprt.c tests/test_drop_chunks.c tests/test_wire.c tests/test_data.c tests/test_kv.c tests/test_meta_apply.c tests/test_raft.c tests/test_sim.c tests/test_txn.c tests/test_session.c tests/test_lock.c
TEST_BINS = tests/test_erasure tests/test_placement tests/test_integration tests/test_quota tests/test_migrate tests/test_directio tests/test_rejoin tests/test_query tests/test_list_exports tests/test_dir_stats tests/test_ino_path tests/test_meta_slot tests/test_add_storage tests/test_meta_cap tests/test_meta_v6 tests/test_rdma_xprt tests/test_drop_chunks tests/test_wire tests/test_data tests/test_kv tests/test_meta_apply tests/test_raft tests/test_sim tests/test_txn tests/test_session tests/test_lock

SERVER_SRCS = src/server/efsd.c src/server/store.c src/server/store_nvme.c \
              src/server/handler.c \
              src/server/cluster.c src/server/meta_server.c src/server/migrate.c \
              src/server/peer_pool.c src/server/writer.c src/server/verify.c \
              src/server/bench_local.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = src/client/efs_fuse.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

BENCH_CLIENT_SRC = src/client/efs_bench.c
BENCH_CLIENT_OBJ = $(BENCH_CLIENT_SRC:.c=.o)

MGMT_SRC = src/mgmt/efs_mgmt.c
MGMT_OBJ = $(MGMT_SRC:.c=.o)

QUERY_SRC = src/query/efs_query.c
QUERY_OBJ = $(QUERY_SRC:.c=.o)

# System fuse3 (>= 3.3.0-19.el8). pkg-config supplies -I and -lfuse3.
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)

BLAKE3_OBJS = $(BLAKE3_DIR)/blake3.o \
              $(BLAKE3_DIR)/blake3_portable.o \
              $(BLAKE3_DIR)/blake3_dispatch.o \
              $(BLAKE3_DIR)/blake3_sse2.o \
              $(BLAKE3_DIR)/blake3_sse41.o \
              $(BLAKE3_DIR)/blake3_avx2.o \
              $(BLAKE3_DIR)/blake3_avx512.o

.PHONY: all clean tests test blake3-bench FORCE

all: $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query tests

# blake3-bench always relinks so a stale binary cannot linger after CPU changes.
FORCE:

test: all
	./tests/test_wire
	./tests/test_data
	./tests/test_kv
	./tests/test_meta_apply
	./tests/test_raft
	./tests/test_sim
	./tests/test_txn
	./tests/test_session
	./tests/test_lock
	./tests/test_erasure
	./tests/test_placement
	./tests/test_integration
	./tests/test_quota
	./tests/test_migrate
	./tests/test_directio
	./tests/test_rejoin
	./tests/test_query
	./tests/test_list_exports
	./tests/test_dir_stats
	./tests/test_ino_path
	./tests/test_meta_slot
	./tests/test_add_storage
	./tests/test_meta_cap
	./tests/test_meta_v6
	./tests/test_drop_chunks
	./tests/test_rw.sh
	./tests/test_find.sh

# Rebuild when public headers change (struct layouts in metadata.h, etc.).
# .build_id.stamp changes content (and mtime) only when the git id changes,
# so every object picks up a moved HEAD or dirty-flag flip — a stale object
# keeping the old -DEFS_BUILD_ID splits the cluster at the HELLO gate.
.build_id.stamp: FORCE
	@echo '$(EFS_GIT_ID)' | cmp -s - $@ 2>/dev/null || echo '$(EFS_GIT_ID)' > $@

$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(MGMT_OBJ) $(QUERY_OBJ): \
	include/efs/common.h include/efs/metadata.h include/efs/protocol.h \
	include/efs/wire.h include/efs/store.h include/efs/transport.h \
	include/efs/kv.h \
	.build_id.stamp

$(LIB): $(COMMON_OBJS)
	ar rcs $@ $^

efsd: $(SERVER_OBJS) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SERVER_OBJS) $(LIB) $(LDFLAGS)

efs-fuse: $(CLIENT_OBJS) $(LIB)
	@pkg-config --exists fuse3 || { \
	  echo "efs-fuse needs fuse3-devel >= 3.3.0 (pkg-config fuse3)" >&2; \
	  exit 1; }
	$(CC) $(CFLAGS) $(INCLUDES) $(FUSE_CFLAGS) -o $@ $(CLIENT_OBJS) $(LIB) $(LDFLAGS) $(FUSE_LIBS)

efs-bench: $(BENCH_CLIENT_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(BENCH_CLIENT_OBJ) $(LIB) $(LDFLAGS)

$(CLIENT_OBJS): CFLAGS += $(FUSE_CFLAGS)

efs-mgmt: $(MGMT_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(MGMT_OBJ) $(LIB) $(LDFLAGS)

efs-query: $(QUERY_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(QUERY_OBJ) $(LIB) $(LDFLAGS)

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

src/sim/sim_lock.o: src/sim/sim_lock.c src/sim/sim_internal.h include/efs/sim.h include/efs/lock.h include/efs/kv_key.h
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/sim -c -o $@ $<

tests/test_sim: tests/test_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ tests/test_sim.c src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o $(LIB) $(LDFLAGS)

%: %.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

blake3-bench: $(BLAKE3_OBJS) FORCE
	$(CC) $(CFLAGS) $(INCLUDES) \
		-o blake3-bench tools/blake3-bench.c $(BLAKE3_OBJS) $(LDFLAGS)

clean:
	rm -f $(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(MGMT_OBJ) $(QUERY_OBJ)
	rm -f src/sim/sim.o src/sim/sim_raft.o src/sim/sim_ctrl.o src/sim/sim_txn.o src/sim/sim_sess.o src/sim/sim_dir.o src/sim/sim_lock.o src/sim/opid.o
	rm -f $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query blake3-bench
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
