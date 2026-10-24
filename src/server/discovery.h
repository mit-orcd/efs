#ifndef EFS_DISCOVERY_H
#define EFS_DISCOVERY_H

/* K33/W157: discovery of a cluster on the local subnet (efsd --join
 * STORAGE). Discovery finds peers only; it never defines membership (M1):
 * the joiner still says HELLO to a peer it found, and the committed record
 * decides. See docs/status/k33-membership-protocol-20261023.md.
 *
 * A seed, promised or formed node answers UDP probes on the discovery
 * port (default EFS_DISCOVERY_PORT, --discovery-port; 0 turns it off). A
 * joiner without a seed sends a probe to the broadcast address of each
 * IPv4 interface that is up, computed from the interface's own netmask
 * (never an assumed /24), or of the one --discover-on names. Each answer
 * carries the cluster identity, whether it is formed, its member count,
 * the service endpoint (so a non-default service port is advertised) and
 * the build. Answers of several clusters require an explicit seed; no
 * answer never creates a cluster.
 *
 * Datagrams (big-endian), magic "EFSDISC" + version 1:
 *   probe:  [magic:8][type=1][nonce:8]
 *   answer: [magic:8][type=2][nonce:8][cluster:8][count:4][formed:1]
 *           [port:2][alen:1][addr:alen][blen:1][build:blen]
 * A datagram of another magic, version or type, a nonce that is not the
 * probe's, a zero cluster or port, or any trailing byte is ignored. */

#include "efs/common.h"
#include <stddef.h>
#include <stdint.h>

#define EFS_DISCOVERY_PORT 17430u
#define EFS_DISC_PROBE_LEN 17u
#define EFS_DISC_ANSWER_MAX (8u + 1u + 8u + 8u + 4u + 1u + 2u + 1u + 63u + 1u + EFS_BUILD_ID_LEN)
#define EFS_DISC_MAX_TARGETS 16

struct efs_disc_answer {
    uint64_t cluster;
    uint32_t count; /* members (formed), or stores in the proposal / joined so far */
    uint8_t formed;
    char addr[64];  /* service endpoint */
    uint16_t port;
    char build_id[EFS_BUILD_ID_LEN];
};

struct efs_disc_target {
    char ifname[32];
    char addr[16];  /* interface address */
    char bcast[16]; /* its subnet's broadcast address */
};

void efs_disc_probe_encode(uint8_t out[EFS_DISC_PROBE_LEN], uint64_t nonce);
/* 0 and *nonce, or -1 for anything but a probe. */
int efs_disc_probe_decode(const uint8_t *in, size_t len, uint64_t *nonce);
/* Encoded length, or 0 when the answer does not fit or is invalid. */
size_t efs_disc_answer_encode(uint8_t *out, size_t cap, uint64_t nonce,
                              const struct efs_disc_answer *a);
/* 0 when in is an answer to nonce, else -1. */
int efs_disc_answer_decode(const uint8_t *in, size_t len, uint64_t nonce,
                           struct efs_disc_answer *a);
/* The broadcast address of addr's subnet under mask (host byte order). */
uint32_t efs_disc_broadcast(uint32_t addr, uint32_t mask);
/* The probe targets: every up IPv4 interface (loopback included), or only
 * `iface` (an interface name or one of its addresses) when it is set. The
 * number found; *names lists them for messages. */
int efs_disc_targets(const char *iface, struct efs_disc_target *t, int max, char *names,
                     size_t cap);
/* Probe every target on port for wait_ms; distinct answers (by endpoint)
 * into out. The number of answers, or -1 when no probe could be sent. */
int efs_disc_probe(const struct efs_disc_target *t, int n, uint16_t port, int wait_ms,
                   struct efs_disc_answer *out, int max);
/* The responder: a thread answering probes on port with what info fills
 * in (it returns 0 to answer, nonzero to stay silent). 0 or -1. */
int efs_disc_responder_start(uint16_t port, int (*info)(struct efs_disc_answer *a));
/* The local IPv4 address this host uses to reach host (a connected UDP
 * socket; nothing is sent). 0 or -1. */
int efs_disc_local_addr(const char *host, uint16_t port, char *out, size_t cap);

#endif
