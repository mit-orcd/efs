
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/socket.h>
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_connect)(int,const struct sockaddr*,socklen_t);
    if (!real_connect) real_connect = dlsym(RTLD_NEXT,"connect");
    if (addr && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in copy = *(const struct sockaddr_in *)addr;
        int port = ntohs(copy.sin_port), base = atoi(getenv("TEST_BASE"));
        if (copy.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && port >= base && port < base+3) {
            copy.sin_port = htons(atoi(getenv("TEST_PROXY"))+atoi(getenv("TEST_NODE"))*3+port-base);
            return real_connect(fd,(const struct sockaddr *)&copy,sizeof(copy));
        }
    }
    return real_connect(fd,addr,len);
}
