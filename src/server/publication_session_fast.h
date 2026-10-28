/* A successful fresh owner check plus the participant's fresh rejection floor
 * proves an already established session without appending another no-op.
 * Missing local establishment still follows the ordered durable command path. */
static int host_session_establish_reuse(struct efs_raft_host *h,
    const uint8_t *cmd,uint32_t clen,uint8_t group,int *hint,int *reused)
{
    *reused=0;
    int rc=host_session_establish_admit(h,cmd,clen,group,hint);
    if(rc)return rc;
    rc=host_read_index(h,group,hint);
    if(rc)return rc;
    rc=efs_session_accept(h->kv,rd32be(cmd+22),cmd+2,rd32be(cmd+18));
    if(rc==EFS_ERR_UNREGISTERED)return EFS_OK;
    if(rc)return rc;
    *reused=1;return EFS_OK;
}
