.DEFAULT_GOAL := all

CC = gcc
# -g keeps symbols for `perf report`; -fno-omit-frame-pointer improves stack
# unwinding under --perf. Both are cheap at -O3.
# Always tune for the build host (compile on the same arch you run on).
# Build id: nodes refuse to cluster across different builds (see HELLO gate).
# Deployment passes an already-verified source identity. Do not rescan its
# filtered remote checkout (including large result artifacts) in that case.
ifndef EFS_GIT_ID
EFS_GIT_ID := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
ifneq ($(shell git status --porcelain 2>/dev/null | head -1),)
EFS_GIT_ID := $(EFS_GIT_ID)-dirty
endif
endif
# Human-facing version string (efs/version.h --version, startup logs, the
# VERSION wire op). Informational only; EFS_BUILD_ID above stays the gate.
ifndef EFS_GIT_DESCRIBE
EFS_GIT_DESCRIBE := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
endif
ifndef EFS_GIT_BRANCH
EFS_GIT_BRANCH   := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
endif
ifndef EFS_BUILD_TIME
EFS_BUILD_TIME   := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
endif
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
ifeq ($(EFS_MEMCHECK),1)
CFLAGS += -DEFS_MEMCHECK=1
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

COMMON_SRCS = src/common/admission.c src/meta/append_plan.c $(COMMON_DIR)/publication.c $(COMMON_DIR)/publication_ack.c \
              $(COMMON_DIR)/common.c $(COMMON_DIR)/packed_segment.c $(COMMON_DIR)/packed_reader.c $(COMMON_DIR)/pack_cohort.c $(COMMON_DIR)/pack_ledger.c \
              $(COMMON_DIR)/log_ts.c \
              $(COMMON_DIR)/batch_obs.c \
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
              src/meta/query_catalog.c src/meta/query_disk.c src/meta/query_policy.c \
              src/meta/name_index.c \
              src/meta/metadata.c \
              src/meta/meta_apply.c src/meta/pack_mapping.c src/meta/pack_admission.c \
              src/meta/txn.c \
              src/meta/session.c \
              src/meta/put_ticket.c src/meta/repair_job.c \
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
              src/client/bufpool.c src/client/writer_state.c src/client/publication_ack_owner.c src/client/publication_writeback.c src/client/pack_write.c src/client/put_ticket_owner.c src/client/put_upload.c \
              src/client/inode_rpc.c \
              src/client/stage_evict.c \
              src/client/node_cache.c

COMMON_OBJS = $(COMMON_SRCS:.c=.o)
LIB = libefs.a

TEST_SRCS = tests/test_repair_job.c tests/test_query_policy.c tests/test_query_refresh.c tests/test_txn_resolve_faults.c tests/test_namespace_session.c tests/test_admission.c tests/test_store_containers.c tests/test_principles_index_fault.c tests/test_principles_index.c tests/test_query_catalog.c tests/test_name_index.c tests/test_append_transaction.c tests/test_store_durability.c tests/test_append_receipt.c tests/test_put_ticket.c tests/test_store_gc.c tests/test_bench_latency.c tests/test_store_overwrite.c tests/test_writer_routing.c tests/test_kv_snapshot_tail.c tests/test_kv_seg_index.c tests/test_mtime_barrier.c tests/test_publication_session.c tests/test_publication_ack.c tests/test_publication_recovery.c tests/test_lane_bootstrap_recovery.c tests/test_fence_view.c tests/test_reply_buffers.c tests/test_bufpool.c tests/test_erasure.c tests/test_placement.c tests/test_rdma_xprt.c tests/test_wire.c tests/test_data.c tests/test_kv.c tests/test_kv_lsm.c tests/test_raft_store.c tests/test_meta_apply.c tests/test_raft.c tests/test_sim.c tests/test_txn.c tests/test_session.c tests/test_lock.c tests/test_stage_evict.c tests/test_conn_fd.c tests/test_query_disk.c tests/test_query_delta.c tests/test_query_disk_io.c
TEST_BINS = tests/test_repair_job tests/test_query_policy tests/test_query_refresh tests/test_txn_resolve_faults tests/test_namespace_session tests/test_admission tests/test_store_containers tests/test_principles_index_fault tests/test_principles_index tests/test_query_catalog tests/test_name_index tests/test_append_transaction tests/test_store_durability tests/test_append_receipt tests/test_put_ticket tests/test_store_gc tests/test_bench_latency tests/test_store_overwrite tests/test_writer_routing tests/test_kv_snapshot_tail tests/test_kv_seg_index tests/test_mtime_barrier tests/test_publication_session tests/test_publication_ack tests/test_publication_recovery tests/test_lane_bootstrap_recovery tests/test_fence_view tests/test_reply_buffers tests/test_bufpool tests/test_erasure tests/test_placement tests/test_rdma_xprt tests/test_wire tests/test_data tests/test_kv tests/test_kv_lsm tests/test_raft_store tests/test_meta_apply tests/test_raft tests/test_sim tests/test_txn tests/test_session tests/test_lock tests/test_stage_evict tests/test_conn_fd tests/test_query_disk tests/test_query_delta tests/test_query_disk_io
TEST_SRCS += tests/test_pack_cohort.c tests/test_packed_reader.c tests/test_packed_publication.c tests/test_packed_upload.c tests/test_pack_admission.c tests/test_pack_snapshot.c tests/test_pack_mapping.c tests/test_packed_metadata.c tests/test_small_segment.c tests/test_packed_segment.c tests/test_packed_store.c tests/test_pack_ledger.c tests/test_pack_ledger_recovery.c tests/test_kv_reader_gate.c
TEST_SRCS += tests/test_retained_namespace_audit.c
TEST_BINS += tests/test_retained_namespace_audit

TEST_BINS += tests/test_pack_cohort tests/test_packed_reader tests/test_packed_publication tests/test_packed_upload tests/test_pack_admission tests/test_pack_snapshot tests/test_pack_mapping tests/test_packed_metadata tests/test_small_segment tests/test_packed_segment tests/test_packed_store tests/test_pack_ledger tests/test_pack_ledger_recovery tests/test_kv_reader_gate

SERVER_SRCS = src/server/repair_worker.c src/server/repair_upload.c src/server/packed_upload.c src/server/put_fence.c src/server/put_upload.c src/server/efsd.c src/server/store.c src/server/store_nvme.c src/server/store_containers.c \
              src/server/handler.c \
              src/server/cluster.c \
              src/server/peer_pool.c src/server/writer.c \
              src/server/iostats.c src/server/thread.c \
              src/server/raft_host.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = src/client/efs_fuse.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

# Simulator objects embed public Raft/KV layouts too. Keep them on the same
# header/build boundary as the library, including newly added sim modules.
SIM_OBJS = $(patsubst %.c,%.o,$(wildcard src/sim/sim*.c))
$(SIM_OBJS): $(wildcard include/efs/*.h) src/sim/sim_internal.h .build_id.stamp

BENCH_CLIENT_SRC = src/client/efs_bench.c src/bench/bench_local.c src/bench/blake3_bench.c src/bench/io_bench.c src/bench/storage_principles.c src/bench/principles_index.c src/bench/storage_churn.c src/bench/storage_ranges.c
# Reuse production storage/writer code without linking daemon/network startup.
# Separate sectioned objects let the linker discard unrelated server functions.
BENCH_STORE_OBJS = src/bench/store.o src/bench/store_nvme.o src/bench/store_containers.o \
                   src/bench/writer.o src/bench/iostats.o src/bench/thread.o
src/bench/%.o: src/server/%.c .build_id.stamp
	$(CC) $(CFLAGS) $(INCLUDES) -DEFS_BENCH_BUILD -ffunction-sections -fdata-sections -c -o $@ $<

BENCH_CLIENT_OBJ = $(BENCH_CLIENT_SRC:.c=.o)
src/bench/bench_local.o src/bench/io_bench.o: src/bench/perf_control.h
src/bench/bench_local.o: src/bench/latency.h
src/bench/principles_index.o: src/bench/principles_index.h
src/bench/storage_principles.o: src/bench/principles_index.h src/bench/storage_principles.h src/bench/perf_control.h src/bench/latency.h
src/bench/storage_churn.o: src/bench/storage_churn.h src/bench/latency.h src/bench/perf_control.h
src/bench/storage_ranges.o: src/bench/storage_ranges.h src/bench/latency.h
src/client/efs_bench.o: src/bench/storage_principles.h src/bench/storage_churn.h src/bench/storage_ranges.h
src/common/admission.o tests/test_admission: include/efs/admission.h

$(BENCH_CLIENT_OBJ): src/bench/bench_local.h src/bench/blake3_bench.h src/bench/io_bench.h
$(SERVER_OBJS) $(BENCH_STORE_OBJS) src/bench/bench_local.o: src/server/server_internal.h
src/server/store.o src/bench/store.o: src/server/data_pressure.h

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
	python3 tests/test_report_retry_budget.py
	python3 tests/test_write_queue.py
	python3 tests/test_conn_checkout_budget.py
	python3 tests/test_net_receive_deadline.py
	python3 tests/test_net_send_deadline.py
	python3 tests/test_connect_deadline.py
	python3 tests/test_resolver_deadline.py
	python3 tests/test_rdma_reply_budget.py
	python3 tests/test_wb_worker_budget.py
	python3 tests/test_wb_drain_budget.py
	python3 tests/test_append_flush_budget.py
	python3 tests/test_append_burst_clock.py
	python3 tests/test_append_reserve_budget.py
	python3 tests/test_put_pool_budget.py
	python3 tests/test_get_pool_startup.py
	python3 tests/test_get_pool_budget.py
	python3 tests/test_get_pair_budget.py
	python3 tests/test_put_retry_budget.py
	python3 tests/test_put_reply_deadline.py
	python3 tests/test_put_tcp_frame.py
	python3 tests/test_put_window_deadline.py
	python3 tests/test_write_budget.py
	python3 tests/test_append_sequence.py
	python3 tests/test_append_session_admission.py
	python3 tests/test_append_session_host.py
	python3 tests/test_session_read_proof.py
	python3 tests/test_pack_authority.py
	python3 tests/test_pack_authority_net_fallback.py
	python3 tests/test_append_host_authority.py
	python3 tests/test_report_orphan.py

test-create-errors: $(LIB)
	python3 tests/test_kv_wal_faults.py
	python3 tests/test_create_errors.py
	python3 tests/test_create_retry_identity.py
	python3 tests/test_create_retry_ack.py
	python3 tests/test_open_lease.py
	python3 tests/test_open_busy.py
	python3 tests/test_unlink_verdict.py
	python3 tests/test_gc_verdict.py
	python3 tests/test_put_hint.py

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
	python3 tests/test_truncate_admission.py
	python3 tests/test_truncate_prefix.py
	python3 tests/test_span_selection.py

test-lookup-memo:
	python3 tests/test_lookup_memo.py
	python3 tests/test_bench_lookup_walk.py

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
	python3 tests/test_stop_dirty_drain.py
	python3 tests/test_fixture_interrupt.py
	python3 tests/test_client_processes.py

test-wb-runtime:
	python3 tests/test_wb_runtime.py
	python3 tests/test_wb_fault.py
	python3 tests/test_stale_recovery.py
	python3 tests/test_replay_fan_budget.py
	python3 tests/test_pull_deadline.py
	python3 tests/test_pull_group_boundary.py
	python3 tests/test_read_ahead_boundary.py
	python3 tests/test_getchunks_budget.py
	python3 tests/test_metadata_retry_budget.py
	python3 tests/test_inode_writer_view_budget.py
	python3 tests/test_readdir_reply_boundary.py
	python3 tests/test_report_frame_boundary.py
	python3 tests/test_report_forward.py
	python3 tests/test_report_session_apply.py
	python3 tests/test_report_activation.py
	python3 tests/test_report_log_coverage.py
	python3 tests/test_host_durable_dispatch.py

test-wb-recovery:
	@set -e; wb_test=$$(mktemp /tmp/efs-test-wb-recovery.XXXXXX); \
	trap 'rm -f "$$wb_test"' EXIT; \
	$(CC) $(CFLAGS) $(INCLUDES) -o "$$wb_test" tests/test_wb_recovery.c; \
	"$$wb_test"

.PHONY: test-client-stop
test-client-stop:
	python3 tests/test_client_stop.py
	python3 tests/test_client_ready.py
	python3 tests/test_stop_dirty_drain.py
	python3 tests/test_fixture_interrupt.py
	python3 tests/test_client_processes.py

all: $(LIB) efsd efs-fuse efs-bench efs-mgmt efs-query tests

# blake3-bench always relinks so a stale binary cannot linger after CPU changes.
FORCE:

# Architecture doc machine-gate (docs/how-it-works/developing.md). Python only;
# safe on the login node. `make test` on a build node runs it too.
docs-check:
	python3 docs/check-architecture.py

test: all
	python3 tests/faults/test_create_retry_fault.py
	python3 tests/faults/test_create_retry_integrated.py
	python3 tests/faults/test_append_cleanup_retry.py
	$(MAKE) tests/test_put_storage_fence
	./tests/test_put_storage_fence
	$(MAKE) tests/test_repair_storage_fence
	./tests/test_repair_storage_fence
	$(MAKE) tests/test_repair_store_identity
	./tests/test_repair_store_identity
	python3 tests/test_storage_fence_members.py
	python3 tests/test_storage_fence_retry.py
	python3 tests/test_session_finish_retry.py
	$(MAKE) tests/test_put_upload_server
	./tests/test_put_upload_server
	$(MAKE) tests/test_put_upload_client
	./tests/test_put_upload_client
	python3 tests/test_applied_interval.py
	python3 tests/test_cursor_checkpoint.py
	./tests/test_store_containers
	python3 tests/test_lookup_errno.py
	python3 tests/test_storage_capacity.py
	python3 tests/test_status_version.py
	python3 tests/test_create_credentials.py
	python3 tests/test_permission_groups.py
	python3 tests/test_chown_permissions.py
	python3 tests/test_readdir_collection.py
	python3 tests/test_readdir_source_cursor.py
	python3 tests/test_access_permissions.py
	python3 tests/test_rename_flags.py
	python3 tests/test_attribute_authority_errno.py
	python3 tests/test_read_errno.py
	python3 tests/test_opendir_authority_errno.py
	python3 tests/test_xattr_names.py
	python3 tests/test_unlink_authority_errno.py
	python3 tests/test_fallocate_errno.py
	python3 tests/test_virtual_export_handles.py
	python3 tests/test_fuse_open_reply.py
	python3 tests/test_fuse_lease_retention.py
	python3 tests/test_inode_lock_stripes.py
	python3 tests/test_create_admission_wait.py
	python3 tests/test_lookup_replacement.py
	python3 tests/test_lookup_read_group.py
	python3 tests/test_readindex_operation.py
	python3 tests/test_readindex_completion.py
	python3 tests/test_readindex_publish.py
	python3 tests/test_readindex_pair.py
	python3 tests/test_readindex_remote_pair.py
	python3 tests/test_read_retry_budget.py
	python3 tests/test_host_read_deadline.py
	python3 tests/test_peer_pool_deadline.py
	python3 tests/test_lookup_group_plan.py
	python3 tests/test_lookup_path_authority.py
	python3 tests/test_read_control_host.py
	python3 tests/test_prepare_cohort.py
	python3 tests/test_append_cohort.py
	python3 tests/test_atomic_append_host.py
	python3 tests/test_host_cover_route.py
	python3 tests/test_namespace_leader_route.py
	python3 tests/test_namespace_admission_rounds.py
	python3 tests/test_session_combined_group.py
	python3 tests/test_namespace_authority.py
	python3 tests/test_w23_samples.py
	python3 tests/test_read_verify.py
	python3 tests/test_opid_admission.py
	python3 tests/test_server_processes.py
	python3 tests/test_mgmt_session_routing.py
	python3 tests/test_session_admission.py
	python3 tests/test_close_effects.py
	python3 tests/test_session_cleanup_cli.py
	python3 tests/test_session_fence_finish.py
	python3 tests/test_publication_host.py
	python3 tests/test_txn_read_authority.py
	python3 tests/test_publication_rpc.py
	python3 tests/test_publication_batch_rpc.py
	python3 tests/test_publication_session_rpc.py
	python3 tests/test_publication_session_reuse.py
	python3 tests/test_lease_edge_rpc.py
	python3 tests/test_lease_request.py
	python3 tests/test_identity_revocation.py
	python3 tests/test_put_ticket_rpc.py
	python3 tests/test_put_ticket_owner.py
	python3 tests/test_put_identity.py
	python3 tests/test_create_request.py
	python3 tests/test_retained_create_authority.py
	python3 tests/test_pending_create_cleanup.py
	python3 tests/test_open_retry_fairness.py
	python3 tests/test_mount_identity_rpc.py
	python3 tests/test_pending_create_command.py
	python3 tests/test_lock_apply_verdict.py
	python3 tests/test_applied_cursor.py
	python3 tests/test_tiny_fragment_audit.py
	python3 tests/test_bench_cli.py
	python3 tests/test_bench_profile.py
	./tests/test_principles_index
	./tests/test_principles_index_fault
	python3 tests/test_storage_principles.py
	python3 tests/test_bench_ranges.py
	python3 tests/test_storage_ranges.py
	python3 tests/test_storage_churn.py
	python3 tests/test_bench_churn.py
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
	python3 tests/test_read_failure_admission.py
	python3 tests/test_read_refs_window.py
	python3 tests/test_query_capacity_mount.py
	python3 tests/test_client_memory.py
	python3 tests/test_rollup_allocation_failure.py
	python3 tests/test_rollup_budget.py
	python3 tests/test_recorder_stop.py
	python3 tests/test_address_parser.py
	python3 tests/test_publication_writeback.py
	python3 tests/test_lane_bootstrap_export.py
	./tests/test_wire
	./tests/test_data
	./tests/test_kv
	./tests/test_admission
	python3 tests/test_metadata_balance.py
	python3 tests/test_host_pressure.py
	python3 tests/test_host_outbox.py
	python3 tests/test_host_flow.py
	python3 tests/test_raft_slow_sync.py
	python3 tests/test_host_inbox.py
	python3 tests/test_host_checkpoint.py
	python3 tests/test_checkpoint_worker.py
	python3 tests/test_host_retire.py
	python3 tests/test_recv_overlap.py
	./tests/test_kv_lsm
	./tests/test_query_policy
	./tests/test_query_catalog
	./tests/test_query_disk
	EFS_QUERY_DELTA_DIRS=1 ./tests/test_query_delta
	EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 ./tests/test_query_delta
	./tests/test_query_disk_io
	./tests/test_query_refresh
	./tests/test_name_index
	./tests/test_bench_latency
	./tests/test_store_overwrite
	./tests/test_store_durability
	$(MAKE) tests/test_repair_file_atomic
	./tests/test_repair_file_atomic
	$(MAKE) tests/test_repair_receiver
	./tests/test_repair_receiver
	$(MAKE) tests/test_repair_inode_gc_race
	./tests/test_repair_inode_gc_race
	$(MAKE) tests/test_repair_worker
	./tests/test_repair_worker
	./tests/test_put_ticket
	./tests/test_repair_job
	python3 tests/test_repair_job_faults.py
	python3 tests/test_repair_host.py
	python3 tests/test_repair_coordinator.py
	python3 tests/test_repair_gc_host.py
	python3 tests/test_repair_source_bounds.py
	./tests/test_store_gc
	./tests/test_store_gc unlimited
	./tests/test_writer_routing
	./tests/test_kv_snapshot_tail
	./tests/test_kv_seg_index
	./tests/test_raft_store
	python3 tests/test_raft_replay_reads.py
	./tests/test_mtime_barrier
	./tests/test_publication_session
	./tests/test_publication_ack
	./tests/test_publication_recovery
	./tests/test_append_receipt
	./tests/test_append_transaction
	python3 tests/test_append_prepare_batch.py
	python3 tests/test_meta_cmd_namespace.py
	python3 tests/test_append_catchup_host.py
	./tests/test_meta_apply
	./tests/test_retained_namespace_audit
	./tests/test_lane_bootstrap_recovery
	./tests/test_raft
	EFS_TEST_EARLY_READ_ACK=1 ./tests/test_raft
	./tests/test_sim
	./tests/test_txn
	./tests/test_txn_resolve_faults
	python3 tests/test_txn_apply_retry.py
	python3 tests/test_publication_persistence.py
	python3 tests/test_follower_read_redirect.py
	python3 tests/test_query_snapshot_failover.py
	python3 tests/test_query_page_sessions.py
	python3 tests/test_query_root_refusals.py
	python3 tests/test_query_capture_retry.py
	python3 tests/test_query_page_barriers.py
	python3 tests/test_query_stats_snapshot.py
	python3 tests/test_query_retained_orphan.py
	python3 tests/test_query_callback_affinity.py
	python3 tests/test_feature_poll_deadline.py
	python3 tests/test_feature_discovery_status.py
	python3 tests/test_close_inspector_scope.py
	python3 tests/test_close_retire_fastpath.py
	python3 tests/test_store_tls.py
	python3 tests/test_report_session_admission.py
	python3 tests/test_namespace_owner_once.py
	python3 tests/test_gc_backlog_idle.py
	python3 tests/test_gc_fragment_backlog.py
	python3 tests/test_gc_inode_fanout.py
	python3 tests/test_tiny_overload_oracle.py
	python3 tests/test_session_register_verdict.py
	python3 tests/test_namespace_session_apply.py
	python3 tests/test_gc_session_admission.py
	python3 tests/test_partition_fence_verdict.py
	python3 tests/test_mkdir_committed_reply.py
	python3 tests/test_namespace_receipt_pending.py
	python3 tests/test_query_loaded_oracle.py
	python3 tests/test_valgrind_verdict.py
	python3 tests/test_txn_parts_bounds.py
	python3 tests/test_prepare_bounds.py
	python3 tests/test_raft_tail.py
	python3 tests/test_query_policy_forward.py
	python3 tests/test_command_policy_mount.py
	python3 tests/test_efs_commands.py
	python3 tests/test_posix_query_budget.py
	python3 tests/test_namespace_bounds.py
	python3 tests/test_namespace_fixture.py
	python3 tests/test_namespace_envelope_fixture.py
	python3 tests/test_posix_phase_trace.py
	python3 tests/test_query_work_trace.py
	python3 tests/test_gc_fixture_drain.py
	python3 tests/test_rename_budget.py
	./tests/test_namespace_session
	./tests/test_session
	./tests/test_erasure
	./tests/test_small_segment
	./tests/test_packed_segment
	./tests/test_packed_metadata
	./tests/test_pack_snapshot
	./tests/test_pack_admission
	./tests/test_packed_upload
	./tests/test_packed_publication
	./tests/test_pack_cohort
	./tests/test_packed_reader
	python3 tests/test_packed_read_budget.py
	python3 tests/test_packed_gc.py
	python3 tests/test_packed_gc_budget.py
	python3 tests/test_mounted_pack_retry.py
	python3 tests/test_mounted_pack_batch.py
	python3 tests/test_mounted_pack_parallel.py
	python3 tests/test_pack_lifecycle_routing.py
	python3 tests/test_packed_writer_view_reuse.py
	python3 tests/test_packed_terminal_publication.py
	python3 tests/test_packed_publication_progress.py
	python3 tests/test_mounted_pack_admission.py
	python3 tests/test_mounted_pack_capability.py
	./tests/test_pack_mapping
	./tests/test_packed_store
	./tests/test_pack_ledger
	./tests/test_pack_ledger_recovery
	./tests/test_kv_reader_gate
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

$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(MGMT_OBJ) $(QUERY_OBJ) $(TEST_SRCS:.c=.o): \
	$(wildcard include/efs/*.h) .build_id.stamp

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
	python3 tests/test_fuse_open_reply.py
	python3 tests/test_fuse_lease_retention.py
	python3 tests/test_inode_lock_stripes.py
	python3 tests/test_create_admission_wait.py
	./tests/test_reply_buffers
	./tests/test_reply_buffers key-failure
	./tests/test_bufpool
	python3 tests/test_read_admission.py
	python3 tests/test_read_failure_admission.py
	python3 tests/test_read_refs_window.py
	python3 tests/test_client_memory.py
	python3 tests/test_rollup_allocation_failure.py
	python3 tests/test_rollup_budget.py
	python3 tests/test_recorder_stop.py
	python3 tests/test_address_parser.py
	python3 tests/test_publication_writeback.py
	python3 tests/test_lane_bootstrap_export.py

$(filter src/client/%.o,$(COMMON_OBJS)) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ): src/client/client_internal.h include/efs/wb_recovery.h
src/client/efs_fuse.o: src/client/reply_buffers.h src/client/stop_control.h
src/raft/raft_disk.o src/raft/raft_log.o: src/raft/raft_disk_internal.h

tests/test_store_durability: tests/test_store_durability.c src/server/store.c src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

tests/test_store_overwrite: tests/test_store_overwrite.c src/server/store.c src/server/data_pressure.h src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

tests/test_store_gc: tests/test_store_gc.c src/server/store.c src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

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
	rm -f $(TEST_BINS) tests/faults/repair_request tests/test_repair_worker tests/test_repair_inode_gc_race tests/test_repair_store_identity tests/test_repair_file_atomic tests/test_repair_receiver tests/test_repair_storage_fence tests/test_put_storage_fence tests/test_put_upload_server tests/test_put_upload_client tests/principles_index_fault.o
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

tests/test_principles_index: tests/test_principles_index.c src/bench/principles_index.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $^ $(LDFLAGS)

tests/test_bench_latency: tests/test_bench_latency.c src/bench/latency.h
	$(CC) $(CFLAGS) -o $@ $<

# Linux/FUSE only: owns private stores and mounts, never the existing cluster.
.PHONY: test-gc-live
GC_MODE ?= direct
GC_ARGS ?= --posix
test-gc-live: all
	python3 tests/live/gc_reclamation.py --mode $(GC_MODE) $(GC_ARGS)

tests/test_put_ticket: tests/test_put_ticket.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile an isolated model with test-only syscall indirection; normal bench
# objects and production libraries never contain these hooks.
tests/principles_index_fault.o: src/bench/principles_index.c src/bench/principles_index.h
	$(CC) $(CFLAGS) $(INCLUDES) -Dfsync=efs_index_test_fsync -Dpwrite=efs_index_test_pwrite -Dreaddir=efs_index_test_readdir -c -o $@ $<

tests/test_principles_index_fault: tests/test_principles_index_fault.c tests/principles_index_fault.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ tests/test_principles_index_fault.c tests/principles_index_fault.o $(LIB) $(LDFLAGS)
src/server/store.o src/server/store_containers.o src/server/efsd.o src/bench/store.o src/bench/store_containers.o: src/server/store_containers.h

# Both sides must rebuild together when GET checksum provenance changes.
src/server/handler.o src/server/store_containers.o src/bench/store_containers.o tests/test_store_containers: include/efs/store.h

tests/test_store_containers: tests/test_store_containers.c src/server/store.c src/server/store_containers.c src/server/server_internal.h src/bench/writer.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o $(LIB) $(LDFLAGS)
# The synthetic scripts are file bytes, embedded without running queries.
src/client/commands/source.h: src/client/commands/command.py src/client/commands/embed.py
	python3 src/client/commands/embed.py
src/client/efs_fuse.o: src/client/commands/source.h include/efs/command_path.h

# Native programs share one help renderer and option reference.
$(SERVER_OBJS) $(CLIENT_OBJS) $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS) $(MGMT_OBJ) $(QUERY_OBJ): include/efs/cli_help.h

tests/test_namespace_session: tests/test_namespace_session.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_txn_resolve_faults: tests/test_txn_resolve_faults.o $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Internal I/O fault regression compiles the actual disk-cache implementation.
src/meta/query_disk.o tests/test_query_disk_io: src/meta/query_disk_incremental.h src/meta/query_disk_budget.h
tests/test_query_disk_io: src/meta/query_disk.c src/meta/query_disk.h

# Independent production storage-session barrier, including syscall failures.
tests/test_put_storage_fence: tests/test_put_storage_fence.c src/server/put_fence.c src/server/server_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

tests/test_put_upload_server: tests/test_put_upload_server.c src/server/put_upload.c src/server/put_fence.c include/efs/put_upload.h include/efs/put_ticket.h src/server/server_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

tests/test_put_upload_client: tests/test_put_upload_client.c src/client/put_upload.c include/efs/put_upload.h include/efs/put_ticket.h src/client/client_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)
# Publication coalescing is shared by the real worker and its policy gate.
src/server/raft_host.o tests/test_query_refresh: src/server/query_refresh.h src/server/query_export_lease.h
src/server/raft_host.o: src/server/read_index_pair_remote.h

# Ticket wire changes must rebuild both endpoints and their actual handler.
src/client/put_upload.o src/server/put_upload.o src/server/handler.o: include/efs/put_upload.h include/efs/put_ticket.h
tests/test_kv_snapshot_tail: src/kv/kv_seg.c src/kv/kv_lsm_internal.h

src/meta/query_catalog.o src/meta/query_disk.o tests/test_query_disk_io: src/meta/query_build_time.h

tests/test_small_segment: tests/test_small_segment.c src/data/erasure.c include/efs/erasure.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ tests/test_small_segment.c src/data/erasure.c

src/common/packed_segment.o: include/efs/packed_segment.h include/efs/erasure.h
tests/test_packed_segment: tests/test_packed_segment.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

src/common/pack_ledger.o: include/efs/pack_ledger.h include/efs/packed_segment.h
tests/test_pack_ledger: tests/test_pack_ledger.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

tests/test_pack_ledger_recovery: tests/test_pack_ledger_recovery.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

tests/test_kv_reader_gate: tests/test_kv_reader_gate.c src/kv/kv_lsm.c src/kv/kv_lsm_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIB) $(LDFLAGS)

src/kv/kv_lsm.o src/kv/kv_wal.o src/kv/kv_compact.o src/kv/kv_seg.o: src/kv/kv_lsm_internal.h

tests/test_packed_upload: tests/test_packed_upload.c src/server/packed_upload.c src/server/put_fence.c src/server/packed_fence.h src/server/store.c src/server/server_internal.h include/efs/packed_upload.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

tests/stress/packed_rpc_gate: tests/stress/packed_rpc_gate.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

tests/stress/pack_storage_soak: tests/stress/pack_storage_soak.c src/server/packed_fence.h src/server/store.c src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

tests/test_packed_store: tests/test_packed_store.c src/server/store.c src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

src/server/store.o src/bench/store.o tests/test_packed_store tests/test_store_durability tests/test_store_overwrite tests/test_store_gc tests/test_store_containers: src/server/packed_fence.h
# Incremental query changes must not fall back solely at 1,025 inode IDs.
tests/test_query_delta: tests/test_query_delta.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# The packed reference extends a shared internal struct. Rebuild every runtime
# consumer so an incremental build cannot retain the old struct ABI.
$(COMMON_OBJS) $(SERVER_OBJS) $(CLIENT_OBJS) $(MGMT_OBJ) $(QUERY_OBJ) $(BENCH_CLIENT_OBJ) $(BENCH_STORE_OBJS): include/efs/meta_apply.h include/efs/packed_segment.h

src/meta/pack_mapping.o: include/efs/pack_mapping.h include/efs/pack_ledger.h include/efs/meta_apply.h include/efs/kv_key.h

tests/test_pack_cohort: tests/test_pack_cohort.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIB) $(LDFLAGS)

src/server/raft_host.o: src/server/packed_gc.h src/server/pack_authority.h

src/server/raft_host.o: src/server/publication_session_fast.h
# Read-only pinned snapshot audit; isolated real-LSM fixtures only.
tests/test_retained_namespace_audit: tests/test_retained_namespace_audit.c tests/retained_namespace_audit.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIB) $(LDFLAGS)

tests/test_repair_job: tests/test_repair_job.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc/kv -o $@ $< $(LIB) $(LDFLAGS)

tests/test_repair_storage_fence: tests/test_repair_storage_fence.c src/server/put_fence.c src/server/server_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

tests/test_repair_file_atomic: tests/test_repair_file_atomic.c src/server/store.c src/server/server_internal.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

tests/test_repair_store_identity: tests/test_repair_store_identity.c src/server/put_fence.c src/server/server_internal.h $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I. -o $@ $< $(LIB) $(LDFLAGS)

# Actual inactive FILES receiver; metadata authority RPCs are explicit adapter
# stubs. No distributed repair or live worker acceptance is inferred.
tests/test_repair_receiver: tests/test_repair_receiver.c src/server/store.c src/server/repair_upload.c src/server/put_fence.c src/server/server_internal.h include/efs/repair_job.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/server/repair_upload.c src/server/put_fence.c src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

# Physical GC/publication race; metadata authority remains an explicit adapter.
tests/test_repair_inode_gc_race: tests/test_repair_inode_gc_race.c src/server/store.c src/server/repair_upload.c src/server/put_fence.c src/server/server_internal.h include/efs/repair_job.h src/bench/writer.o src/bench/store_containers.o $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< src/server/repair_upload.c src/server/put_fence.c src/bench/writer.o src/bench/store_containers.o $(LIB) $(LDFLAGS)

# Explicit private experimental repair fixture driver.
tests/faults/repair_request: tests/faults/repair_request.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIB) $(LDFLAGS)

tests/test_repair_worker: tests/test_repair_worker.c src/server/repair_worker.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< src/server/repair_worker.c $(LIB) $(LDFLAGS)

# Broad subtree queries share the bounded immutable-KV traversal.
src/meta/query_catalog.o src/meta/query_disk.o: src/meta/query_walk.h
