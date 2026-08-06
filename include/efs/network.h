#ifndef EFS_NETWORK_H
#define EFS_NETWORK_H

#include <stddef.h>
#include <stdint.h>

/* Create a TCP connection to host:port. Returns fd or -1 on error. */
int efs_connect_tcp(const char *host, uint16_t port);

/* Bind and listen on host:port. Returns fd or -1 on error. */
int efs_listen_tcp(const char *host, uint16_t port, int backlog);

/* Set receive timeout in milliseconds. */
int efs_set_recv_timeout(int fd, int ms);

/* Set send timeout in milliseconds. */
int efs_set_send_timeout(int fd, int ms);

/* Send exactly len bytes. Returns 0 on success, -1 on error. */
int efs_send_all(int fd, const void *buf, size_t len);

/* Receive exactly len bytes. Returns 0 on success, -1 on error/EOF. */
int efs_recv_all(int fd, void *buf, size_t len);

#endif
