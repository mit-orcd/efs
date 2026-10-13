/* Bounded traversal of a requested subtree, using its immutable KV view.
 * The caller's KV adapter charges physical scans against one shared budget.
 * Frames are bounded by the path limit, never by catalog cardinality. */
#ifndef EFS_QUERY_WALK_H
#define EFS_QUERY_WALK_H
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct query_walk_frame {
    efs_ino_t ino;
    struct efs_meta_dir_cursor cursor;
    size_t length;
};
static int query_walk(struct efs_kv *kv,efs_ino_t root,enum efs_name_match match,
    const char *term,int (*emit)(void *,const struct efs_name_entry *,const char *),void *user)
{
    if (strnlen(term,EFS_MAX_NAME)>=EFS_MAX_NAME) return EFS_ERR_INVAL;
    struct query_walk_frame *frames=calloc(EFS_MAX_PATH,sizeof(*frames));
    if (!frames) return EFS_ERR_NOMEM;
    char path[EFS_MAX_PATH];path[0]=0;
    size_t depth=0;frames[0].ino=root;int rc=EFS_OK;
    while (1) {
        struct query_walk_frame *f=&frames[depth];
        if (f->cursor.done) {
            if (!depth) break;
            depth--;path[frames[depth].length]=0;continue;
        }
        struct efs_meta_dir_ent entry;uint32_t n=0;
        rc=efs_meta_apply_readdir(kv,f->ino,&f->cursor,&entry,1,&n);
        if (rc) break;
        if (!n) continue;
        size_t len=f->length,nl=strlen(entry.name),tl=strlen(term);
        if (len+!!len+nl>=sizeof(path)) { rc=EFS_ERR_NAMETOOLONG;break; }
        if (len) path[len++]='/';
        memcpy(path+len,entry.name,nl+1);
        int matches=match==EFS_NAME_CONTAINS ? strstr(entry.name,term)!=NULL :
            nl>=tl && !memcmp(entry.name+nl-tl,term,tl);
        struct efs_name_entry e={.parent=f->ino,.dentry=entry.d};
        memcpy(e.name,entry.name,sizeof(e.name));
        if (matches && (rc=emit(user,&e,path))) break;
        struct efs_meta_row row;
        rc=efs_meta_apply_get_inode(kv,entry.d.ino,&row);
        if (rc) break;
        if (S_ISDIR(row.mode)) {
            if (depth+1>=EFS_MAX_PATH) { rc=EFS_ERR_NAMETOOLONG;break; }
            frames[++depth]=(struct query_walk_frame){.ino=entry.d.ino,.length=len+nl};
        } else path[f->length]=0;
    }
    free(frames);return rc;
}
#endif
