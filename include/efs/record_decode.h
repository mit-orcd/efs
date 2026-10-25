#ifndef EFS_RECORD_DECODE_H
#define EFS_RECORD_DECODE_H
#include "efs/protocol.h"
#include "efs/put_ticket.h"
/* Pure input validation. No I/O, admission, KV mutation or caller-owned
 * output changes on rejection. All sizes are the current format only. */
int efs_peer_hello_decode(const void *, uint32_t, struct efs_msg_hello *);
int efs_recovery_scan_request_decode(const void *, uint32_t, struct efs_msg_recovery_scan *);
int efs_recovery_scan_reply_decode(const void *, uint32_t, uint32_t what,
                                  struct efs_msg_recovery_scan_reply *, uint8_t *records);
int efs_degraded_mark_decode(const uint8_t *, uint32_t, struct efs_meta_degraded *);
int efs_cleanup_probe_decode(const uint8_t *, uint32_t, struct efs_opid *, uint32_t *, uint64_t *);
struct efs_audit_request { uint32_t group, flags, count; uint64_t incarnation; };
struct efs_audit_reply {
    int32_t rc;
    uint32_t count, audited, settled;
    uint64_t incarnation;
    uint8_t status[EFS_SEGMENT_AUDIT_MAX];
};
int efs_segment_audit_request_decode(const uint8_t *, uint32_t, struct efs_audit_request *);
int efs_segment_audit_reply_decode(const uint8_t *, uint32_t, struct efs_audit_reply *);
#endif
