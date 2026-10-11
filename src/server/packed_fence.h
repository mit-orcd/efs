/* Included only by store.c. Typed packed RPCs use these W95 objects; mounted
 * packed writers remain gated.
 * Generation-specific certificates remain until an authoritative admission
 * floor excludes every old PUT. They never share inode death-page identity. */
static pthread_rwlock_t packed_gc_locks[FRAG_LOCKS];
static pthread_once_t packed_gc_once=PTHREAD_ONCE_INIT;
static void packed_gc_init(void)
{for(unsigned i=0;i<FRAG_LOCKS;i++)pthread_rwlock_init(&packed_gc_locks[i],NULL);}
static pthread_rwlock_t *packed_gc_lock(efs_export_id_t eid,uint64_t id,uint64_t gen)
{
    pthread_once(&packed_gc_once,packed_gc_init);
    return &packed_gc_locks[(id^(id>>32)^gen^(gen>>32)^eid)&(FRAG_LOCKS-1)];
}
static int packed_fence_path(struct efsd_server *s,efs_export_id_t eid,
    uint64_t id,uint64_t gen,char *path,size_t capacity)
{
    if(!s || !eid || !id || !gen)return EFS_ERR_INVAL;
    const char *root=s->storage_path_count?s->storage_paths[0]:s->storage_path;
    int n=snprintf(path,capacity,"%s/meta/pack-fences-v1/%u/%llu/%llu",root,eid,
        (unsigned long long)id,(unsigned long long)gen);
    return n<0 || (size_t)n>=capacity?EFS_ERR_OVERFLOW:EFS_OK;
}
static void packed_fence_bytes(efs_export_id_t eid,uint64_t id,uint64_t gen,uint8_t out[64])
{
    memset(out,0,64);memcpy(out,"EFSDEAD1",8);
    for(unsigned i=0;i<4;i++)out[8+i]=(uint8_t)(eid>>(24-8*i));
    for(unsigned i=0;i<8;i++) {
        out[16+i]=(uint8_t)(id>>(56-8*i));out[24+i]=(uint8_t)(gen>>(56-8*i));
    }
    efs_hash(out,32,out+32);
}
/* 0 absent, 1 matching certificate, negative failure. A corrupt/partial fence
 * denies PUT. Matching bytes alone are not a durable fence ACK; the setter
 * repeats file and ancestor barriers on every retry. Shared lock held. */
static int packed_fence_read(const char *path,const uint8_t expected[64])
{
    int fd;do{fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);}while(fd<0 && errno==EINTR);
    if(fd<0)return errno==ENOENT?0:EFS_ERR_IO;
    struct stat st;uint8_t bytes[64];size_t got=0;int rc=1;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=sizeof(bytes))rc=EFS_ERR_IO;
    while(rc==1 && got<sizeof(bytes)) {
        ssize_t n=pread(fd,bytes+got,sizeof(bytes)-got,(off_t)got);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=EFS_ERR_IO;break;}got+=(size_t)n;
    }
    if(close(fd))rc=EFS_ERR_IO;
    if(rc==1 && memcmp(bytes,expected,sizeof(bytes)))rc=EFS_ERR_IO;
    return rc;
}
int server_fence_packed_object(struct efsd_server *s,efs_export_id_t eid,uint64_t id,uint64_t gen)
{
#ifdef EFS_BENCH_BUILD
    (void)s;(void)eid;(void)id;(void)gen;return EFS_ERR_INVAL;
#else
    char path[8192];uint8_t bytes[64];
    if(!s || s->storage_engine)return EFS_ERR_INVAL;
    int rc=packed_fence_path(s,eid,id,gen,path,sizeof(path));if(rc)return rc;
    packed_fence_bytes(eid,id,gen,bytes);pthread_rwlock_t *mu=packed_gc_lock(eid,id,gen);
    pthread_rwlock_wrlock(mu);
    int present=packed_fence_read(path,bytes);
    if(present<0){pthread_rwlock_unlock(mu);return present;}
    if(!present)make_dir_for_file(path);
    int fd;do{fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW|(present?0:O_CREAT|O_EXCL),0600);}while(fd<0 && errno==EINTR);
    if(fd<0)rc=EFS_ERR_IO;
    else {
        if(!present) {
            struct iovec iov={bytes,sizeof(bytes)};
            if(writev_all(fd,&iov,1))rc=EFS_ERR_IO;
        }
        int sr;do{sr=fsync(fd);}while(sr<0 && errno==EINTR);
        int cr=close(fd);if(sr<0 || cr<0)rc=EFS_ERR_IO;
        /* Keep certificate publication's existing full-ancestry barrier;
         * the scoped candidate changes fragment PUT paths only. */
        if(!rc)rc=sync_fragment_parents(NULL,path);
    }
    pthread_rwlock_unlock(mu);return rc;
#endif
}
int server_write_packed_fragment(struct efsd_server *s,efs_export_id_t eid,
    uint64_t id,uint64_t gen,uint32_t fi,const uint8_t *data,uint32_t len,
    const uint8_t sum[EFS_HASH_SIZE])
{
    char path[8192];uint8_t bytes[64];
    if(!s || s->storage_engine || fi>=EFS_NUM_FRAGMENTS || !data || !sum ||
       !packed_fragment_length_valid(len))return EFS_ERR_INVAL;
    int rc=packed_fence_path(s,eid,id,gen,path,sizeof(path));if(rc)return rc;
    packed_fence_bytes(eid,id,gen,bytes);pthread_rwlock_t *mu=packed_gc_lock(eid,id,gen);
    pthread_rwlock_rdlock(mu);int dead=packed_fence_read(path,bytes);
    rc=dead ? (dead>0 ? EFS_ERR_STALE : dead) :
        write_packed_fragment_unfenced(s,eid,id,gen,fi,data,len,sum);
    pthread_rwlock_unlock(mu);return rc;
}

/* Internal GC entry point: the caller must already prove every captured
 * member's durable fence ACK in authoritative shared-object metadata. A local
 * certificate is necessary, but does not manufacture that distributed proof. */
int server_delete_packed_fragment(struct efsd_server *s, efs_export_id_t eid,
    uint64_t id, uint64_t gen, uint32_t fi, uint32_t len,
    const uint8_t expected_sum[EFS_HASH_SIZE])
{
#ifdef EFS_BENCH_BUILD
    (void)s; (void)eid; (void)id; (void)gen; (void)fi; (void)len; (void)expected_sum;
    return EFS_ERR_INVAL;
#else
    char fence[8192]; uint8_t certificate[64];
    if (!s || s->storage_engine || fi >= EFS_NUM_FRAGMENTS || !expected_sum ||
        !packed_fragment_length_valid(len)) return EFS_ERR_INVAL;
    int rc = packed_fence_path(s, eid, id, gen, fence, sizeof(fence));
    if (rc != EFS_OK) return rc;
    packed_fence_bytes(eid, id, gen, certificate);
    int dead = packed_fence_read(fence, certificate);
    if (dead != 1) return dead < 0 ? dead : EFS_ERR_BUSY;
    /* Visible certificate bytes alone are insufficient after an uncertain
     * fence write. Re-establish file/ancestor durability before unlink. */
    rc = server_fence_packed_object(s, eid, id, gen);
    if (rc != EFS_OK) return rc;
    pthread_rwlock_t *mu = packed_gc_lock(eid, id, gen);
    pthread_rwlock_wrlock(mu);
    uint8_t *data = NULL, actual[EFS_HASH_SIZE]; uint32_t capacity = len;
    if (posix_memalign((void **)&data, 4096, len)) { rc = EFS_ERR_NOMEM; goto done; }
    rc = server_read_packed_fragment(s, eid, id, gen, fi, len, data, &capacity, actual);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) goto done;
    int present = rc == EFS_OK;
    if (present && memcmp(actual, expected_sum, EFS_HASH_SIZE)) { rc = EFS_ERR_CHECKSUM; goto done; }
    uint32_t roots = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t root = 0; root < roots; root++) {
        char path[8192]; struct stat st;
        rc = server_packed_fragment_path(s, root, eid, id, gen, fi, path, sizeof(path));
        if (rc != EFS_OK) goto done;
        if (lstat(path, &st)) {
            if (errno != ENOENT) { rc = EFS_ERR_IO; goto done; }
        } else {
            if (!present || !S_ISREG(st.st_mode) ||
                (st.st_size != (off_t)len + EFS_HASH_SIZE && st.st_size != (off_t)len + 4096)) {
                rc = EFS_ERR_IO; goto done;
            }
            if (gc_unlink_file(s, path)) { rc = EFS_ERR_IO; goto done; }
            __atomic_fetch_add(&s->gc_removed_fragments, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s->gc_removed_file_bytes, (uint64_t)st.st_size, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s->gc_removed_payload_bytes, len, __ATOMIC_RELAXED);
            __atomic_store_n(&s->gc_last_delete_us, gc_clock_us(), __ATOMIC_RELAXED);
            /* Account exactly the actual unlink, even if its subsequent
             * directory barrier fails. An absent retry never subtracts twice;
             * cold inventory accounts any crash-reappearing object again. */
            pthread_mutex_lock(&s->lock);
            struct efs_node *local = server_local_node(s);
            if (local) local->used = local->used >= len ? local->used - len : 0;
            pthread_mutex_unlock(&s->lock);
            server_usage_mark_dirty(s);
        }
        rc = fragment_parent_sync(s, path);
        if (rc != EFS_OK) goto done;
    }
    rc = EFS_OK;
done:
    free(data); pthread_rwlock_unlock(mu);
    if (rc != EFS_OK) __atomic_fetch_add(&s->gc_delete_errors, 1, __ATOMIC_RELAXED);
    return rc;
#endif
}
