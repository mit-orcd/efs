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
         -DEFS_BUILD_ID='"$(EFS_GIT_ID)"'
INCLUDES = -Iinclude -Isrc/common -Ideps/blake3

LDFLAGS = -lpthread -lm -ldl

COMMON_DIR = src/common
BLAKE3_DIR = deps/blake3

COMMON_SRCS = $(COMMON_DIR)/common.c \
              $(COMMON_DIR)/protocol.c \
              $(COMMON_DIR)/metadata.c \
              $(COMMON_DIR)/placement.c \
              $(COMMON_DIR)/erasure.c \
              $(COMMON_DIR)/checksum.c \
              $(COMMON_DIR)/network.c \
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
              src/client/node_cache.c

COMMON_OBJS = $(COMMON_SRCS:.c=.o)
LIB = libefs.a

TEST_SRCS = tests/test_erasure.c tests/test_placement.c tests/test_integration.c tests/test_quota.c tests/test_migrate.c tests/test_directio.c tests/test_rejoin.c tests/test_query.c tests/test_list_exports.c tests/test_dir_stats.c tests/test_ino_path.c tests/test_meta_slot.c
TEST_BINS = tests/test_erasure tests/test_placement tests/test_integration tests/test_quota tests/test_migrate tests/test_directio tests/test_rejoin tests/test_query tests/test_list_exports tests/test_dir_stats tests/test_ino_path tests/test_meta_slot

SERVER_SRCS = src/server/efsd.c src/server/store.c src/server/handler.c \
              src/server/cluster.c src/server/meta_server.c src/server/migrate.c \
              src/server/peer_pool.c src/server/writer.c src/server/bench_local.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = src/client/efs_fuse.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

BENCH_CLIENT_SRC = src/client/efs_bench.c
BENCH_CLIENT_OBJ = $(BENCH_CLIENT_SRC:.c=.o)

MGMT_SRC = src/mgmt/efs_mgmt.c
MGMT_OBJ = $(MGMT_SRC:.c=.o)

QUERY_SRC = src/query/efs_query.c
QUERY_OBJ = $(QUERY_SRC:.c=.o)

FUSE_DIR = deps/libfuse
FUSE_CFLAGS = -I$(FUSE_DIR)/include -D_FILE_OFFSET_BITS=64
FUSE_LIBS = $(FUSE_DIR)/lib/.libs/libfuse.a
# Vendored libfuse trips gcc truncation/fallthrough warnings; quiet those and
# skip example apps we never install. Reconfigure when this Makefile changes.
FUSE_CONFIGURE_FLAGS = --disable-util --disable-example
FUSE_BUILD_CFLAGS = -O3 -g -fno-omit-frame-pointer -march=native -mtune=native \
	-Wno-stringop-truncation -Wno-implicit-fallthrough -Wno-unused-result

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
	./tests/test_rw.sh

# Rebuild when public headers change (struct layouts in metadata.h, etc.).
$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(MGMT_OBJ) $(QUERY_OBJ): \
	include/efs/common.h include/efs/metadata.h include/efs/protocol.h

$(LIB): $(COMMON_OBJS)
	ar rcs $@ $^

efsd: $(SERVER_OBJS) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SERVER_OBJS) $(LIB) $(LDFLAGS)

efs-fuse: $(CLIENT_OBJS) $(LIB) $(FUSE_LIBS)
	$(CC) $(CFLAGS) $(INCLUDES) $(FUSE_CFLAGS) -o $@ $(CLIENT_OBJS) $(LIB) $(LDFLAGS) $(FUSE_LIBS)

efs-bench: $(BENCH_CLIENT_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(BENCH_CLIENT_OBJ) $(LIB) $(LDFLAGS)

$(CLIENT_OBJS): CFLAGS += $(FUSE_CFLAGS)

efs-mgmt: $(MGMT_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(MGMT_OBJ) $(LIB) $(LDFLAGS)

efs-query: $(QUERY_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(QUERY_OBJ) $(LIB) $(LDFLAGS)

tests: $(TEST_BINS)

%: %.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

blake3-bench: $(BLAKE3_OBJS) FORCE
	$(CC) $(CFLAGS) $(INCLUDES) \
		-o blake3-bench tools/blake3-bench.c $(BLAKE3_OBJS) $(LDFLAGS)

$(FUSE_DIR)/.efs-configured: Makefile
	cd $(FUSE_DIR) && ./configure $(FUSE_CONFIGURE_FLAGS) \
		CFLAGS="$(FUSE_BUILD_CFLAGS)"
	touch $@

$(FUSE_LIBS): $(FUSE_DIR)/.efs-configured
	cd $(FUSE_DIR) && $(MAKE)

clean:
	rm -f $(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(MGMT_OBJ) $(QUERY_OBJ)
	rm -f $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query blake3-bench
	rm -f $(TEST_BINS)
	rm -f $(FUSE_DIR)/.efs-configured
	if [ -f $(FUSE_DIR)/Makefile ]; then cd $(FUSE_DIR) && $(MAKE) clean; fi

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
