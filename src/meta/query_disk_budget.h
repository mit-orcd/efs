/* Serialize cache builders, not readers. Reconstruct retained usage from disk
 * before admitting a candidate: no namespace-sized ledger and no optimistic
 * process-local counter surviving a crash. Count shared links conservatively
 * per generation. A disappearing file is a concurrent lease release. */
static int root_budget_acquire(struct efs_query_disk *d,const char *root)
{
    int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return EFS_ERR_IO;
    int lock=openat(dir,".budget.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0){close(dir);return EFS_ERR_IO;}
    if(flock(lock,LOCK_EX|LOCK_NB)){
        int rc=errno==EWOULDBLOCK?EFS_ERR_BUSY:EFS_ERR_IO;
        close(lock);close(dir);return rc;
    }
    DIR *views=fdopendir(dup(dir));int rc=views?EFS_OK:EFS_ERR_IO;
    uint64_t used=0;
    while(!rc){
        errno=0;struct dirent *entry=readdir(views);
        if(!entry){if(errno)rc=EFS_ERR_IO;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(strncmp(entry->d_name,"view-",5)){
            struct stat st;
            if(fstatat(dir,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){
                if(errno==ENOENT)continue;
                rc=EFS_ERR_IO;break;
            }
            if(S_ISLNK(st.st_mode) && (!strcmp(entry->d_name,"current") || !strcmp(entry->d_name,"current.new")))continue;
            if(!S_ISREG(st.st_mode) || st.st_size<0){rc=EFS_ERR_PROTO;break;}
            uint64_t bytes=(uint64_t)st.st_size;
            if(bytes>UINT64_MAX-used){rc=EFS_ERR_OVERFLOW;break;}
            used+=bytes;
            if(used>aggregate_limit){rc=EFS_ERR_QUOTA;break;}
            continue;
        }
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        int child=openat(dir,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0){if(errno==ENOENT)continue;rc=EFS_ERR_IO;break;}
        DIR *files=fdopendir(dup(child));if(!files)rc=EFS_ERR_IO;
        while(!rc){
            errno=0;struct dirent *file=readdir(files);
            if(!file){if(errno)rc=EFS_ERR_IO;break;}
            if(!strcmp(file->d_name,".") || !strcmp(file->d_name,".."))continue;
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            struct stat st;
            if(fstatat(child,file->d_name,&st,AT_SYMLINK_NOFOLLOW)){
                if(errno==ENOENT)continue;
                rc=EFS_ERR_IO;break;
            }
            if(!S_ISREG(st.st_mode) || st.st_size<0){rc=EFS_ERR_PROTO;break;}
            uint64_t bytes=(uint64_t)st.st_size;
            if(bytes>UINT64_MAX-used){rc=EFS_ERR_OVERFLOW;break;}
            used+=bytes;
            if(used>aggregate_limit){rc=EFS_ERR_QUOTA;break;}
        }
        if(files)closedir(files);
        close(child);
    }
    if(views)closedir(views);
    close(dir);
    if(!rc){
        d->root_bytes=used;d->root_limit=aggregate_limit;
        rc=charge_disk(d,0);
    }
    if(rc)close(lock);else d->budget_fd=lock;
    return rc;
}
