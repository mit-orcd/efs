static int persist_applied(struct efs_raft_host *h, int gi, uint64_t idx,
                           uint64_t now, int force)
{
    uint8_t buf[16];
    int n;

    if (!h->g[gi].hosted || !h->g[gi].r)
        return EFS_OK;
    if (idx == 0 || idx == h->g[gi].applied_saved)
        return EFS_OK;
    if (!force && now - h->g[gi].applied_saved_us < HOST_APPLIED_PERSIST_US)
        return EFS_OK;
    h->g[gi].applied_saved_us = now;
    if (h->g[gi].applied_fd < 0) {
        char path[EFS_MAX_PATH];

        applied_path(h, h->g[gi].group, path, sizeof(path));
        h->g[gi].applied_fd = open(path, O_RDWR | O_CREAT, 0644);
        if (h->g[gi].applied_fd < 0)
            return EFS_ERR_IO;
    }
    wr64be(buf, idx);
    wr64be(buf + 8, ~idx);
    n = (int)pwrite(h->g[gi].applied_fd, buf, sizeof(buf), 0);
    if (n != (int)sizeof(buf) || fdatasync(h->g[gi].applied_fd) != 0) {
        close(h->g[gi].applied_fd);
        h->g[gi].applied_fd = -1;
        return EFS_ERR_IO;
    }
    h->g[gi].applied_saved = idx;
    return EFS_OK;
}
