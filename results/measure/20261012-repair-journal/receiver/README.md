# Inactive receiver evidence

Source cc090985 plus next receiver commit's Makefile, server_internal.h, repair_upload.c and test_repair_receiver.c. Private complete archive `/home/efs/nuc-receiver-cc090985-wip`, build ID cc090985-receiver-wip; no installed NUC binary/service changed. `make -j8 EFS_GIT_ID=cc090985-receiver-wip all tests/test_repair_receiver` and receiver exit0.

Actual receiver test includes production store.c with storage failure interceptors, links production put_fence.c, repair_upload.c and repair_job.c; metadata authority/writer-view RPCs are explicit stubs. Real buffered/direct FILES payload/hash, quota, identity, target certificate and cold helper checked; this is not a live daemon restart, election or second-survivor-loss oracle. ASan/UBSan exit0; baseline frozen library helpers uninstrumented, leak checking disabled (`ASAN_OPTIONS=detect_leaks=0`). Existing unrelated prepare_storage_root format-truncation warnings retained.

Initial build lacked store dependency header path; second separate-source compilation lacked production -D_GNU_SOURCE (O_TMPFILE refused). Both failures retained, corrected production Makefile build passes. An earlier receiver syntax check exposed pointer/array prototype mismatch, corrected before this checkpoint.

Manual sanitizer command from scratch root:

```
cc -D_GNU_SOURCE -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections -Iinclude -I/home/efs/nuc-28-qualification/source-normal/src/bench -I/home/efs/nuc-28-qualification/source-normal/src/server -Wl,--gc-sections tests/test_repair_receiver.c src/server/repair_upload.c src/server/put_fence.c src/meta/repair_job.c /home/efs/nuc-28-qualification/source-normal/src/bench/writer.o /home/efs/nuc-28-qualification/source-normal/src/bench/store_containers.o /home/efs/nuc-28-qualification/source-normal/libefs.a -lpthread -lm -ldl -libverbs -o tests/test_repair_receiver_san
ASAN_OPTIONS=detect_leaks=0 tests/test_repair_receiver_san
```
