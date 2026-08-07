#include "efs/numa_locality.h"
#include "efs/network.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int efs_numa_parse_cpulist(const char *text, cpu_set_t *set)
{
    if (!text || !set)
        return -1;
    CPU_ZERO(set);
    const char *p = text;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (!isdigit((unsigned char)*p))
            return -1;
        char *end = NULL;
        long a = strtol(p, &end, 10);
        if (end == p || a < 0 || a >= CPU_SETSIZE)
            return -1;
        p = end;
        long b = a;
        if (*p == '-') {
            p++;
            if (!isdigit((unsigned char)*p))
                return -1;
            b = strtol(p, &end, 10);
            if (end == p || b < 0 || b >= CPU_SETSIZE || b < a)
                return -1;
            p = end;
        }
        for (long c = a; c <= b; c++)
            CPU_SET((int)c, set);
    }
    return CPU_COUNT(set) > 0 ? 0 : -1;
}

int efs_numa_intersect_process_affinity(cpu_set_t *set)
{
    if (!set)
        return -1;
    cpu_set_t proc;
    CPU_ZERO(&proc);
    if (sched_getaffinity(0, sizeof(proc), &proc) != 0)
        return -1;
    cpu_set_t out;
    CPU_AND(&out, set, &proc);
    *set = out;
    return CPU_COUNT(set) > 0 ? 0 : -1;
}

int efs_numa_cpus_for_node(int node, cpu_set_t *set)
{
    if (!set || node < 0)
        return -1;
    char path[256];
    snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    if (efs_numa_parse_cpulist(buf, set) != 0)
        return -1;
    return efs_numa_intersect_process_affinity(set);
}

static int read_numa_node_file(const char *sysfs_dir, int *node_out)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/numa_node", sysfs_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int node = -1;
    if (fscanf(f, "%d", &node) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (node < 0)
        return -1;
    *node_out = node;
    return 0;
}

static int resolve_block_sysfs(dev_t dev, char *out, size_t out_len)
{
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(dev), minor(dev));
    char resolved[PATH_MAX];
    if (!realpath(link, resolved))
        return -1;
    if (snprintf(out, out_len, "%s", resolved) >= (int)out_len)
        return -1;
    return 0;
}

static int numa_from_block_sysfs(const char *block_sysfs, int *node_out)
{
    char cur[PATH_MAX];
    if (snprintf(cur, sizeof(cur), "%s", block_sysfs) >= (int)sizeof(cur))
        return -1;

    for (int depth = 0; depth < 8; depth++) {
        if (read_numa_node_file(cur, node_out) == 0)
            return 0;

        char device[PATH_MAX];
        snprintf(device, sizeof(device), "%s/device", cur);
        if (read_numa_node_file(device, node_out) == 0)
            return 0;

        char slaves[PATH_MAX];
        snprintf(slaves, sizeof(slaves), "%s/slaves", cur);
        DIR *d = opendir(slaves);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.')
                    continue;
                char slave[PATH_MAX];
                snprintf(slave, sizeof(slave), "%s/%s", slaves, ent->d_name);
                char slave_real[PATH_MAX];
                if (realpath(slave, slave_real)) {
                    if (read_numa_node_file(slave_real, node_out) == 0) {
                        closedir(d);
                        return 0;
                    }
                    char slave_dev[PATH_MAX];
                    snprintf(slave_dev, sizeof(slave_dev), "%s/device", slave_real);
                    if (read_numa_node_file(slave_dev, node_out) == 0) {
                        closedir(d);
                        return 0;
                    }
                }
                break;
            }
            closedir(d);
        }

        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur)
            break;
        *slash = '\0';
    }
    return -1;
}

int efs_numa_for_path(const char *path, int *node_out, cpu_set_t *set)
{
    if (node_out)
        *node_out = -1;
    if (set)
        CPU_ZERO(set);
    if (!path || !node_out || !set)
        return -1;

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    char sysfs[PATH_MAX];
    if (resolve_block_sysfs(st.st_dev, sysfs, sizeof(sysfs)) != 0)
        return -1;

    int node = -1;
    if (numa_from_block_sysfs(sysfs, &node) != 0)
        return -1;
    *node_out = node;
    if (efs_numa_cpus_for_node(node, set) != 0) {
        *node_out = -1;
        CPU_ZERO(set);
        return -1;
    }
    return 0;
}

static int numa_from_netif(const char *ifname, int *node_out)
{
    if (!ifname || !*ifname || !node_out)
        return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
    if (read_numa_node_file(path, node_out) == 0)
        return 0;

    /* bond/vlan: try master */
    snprintf(path, sizeof(path), "/sys/class/net/%s/master", ifname);
    char master_link[PATH_MAX];
    if (realpath(path, master_link)) {
        const char *mname = strrchr(master_link, '/');
        mname = mname ? mname + 1 : master_link;
        snprintf(path, sizeof(path), "/sys/class/net/%s/device", mname);
        if (read_numa_node_file(path, node_out) == 0)
            return 0;
    }

    /* bond: first slave */
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding/slaves", ifname);
    FILE *f = fopen(path, "r");
    if (f) {
        char slaves[256];
        if (fgets(slaves, sizeof(slaves), f)) {
            char *tok = strtok(slaves, " \t\n");
            if (tok && *tok) {
                snprintf(path, sizeof(path), "/sys/class/net/%s/device", tok);
                if (read_numa_node_file(path, node_out) == 0) {
                    fclose(f);
                    return 0;
                }
            }
        }
        fclose(f);
    }
    return -1;
}

static int ifname_for_local_addr(const struct sockaddr *local, char *ifname, size_t ifname_len)
{
    if (!local || !ifname || ifname_len == 0)
        return -1;

    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0)
        return -1;

    int found = -1;
    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        if (ifa->ifa_addr->sa_family != local->sa_family)
            continue;
        if (local->sa_family == AF_INET) {
            const struct sockaddr_in *a = (const struct sockaddr_in *)local;
            const struct sockaddr_in *b = (const struct sockaddr_in *)ifa->ifa_addr;
            if (a->sin_addr.s_addr == b->sin_addr.s_addr) {
                snprintf(ifname, ifname_len, "%s", ifa->ifa_name);
                found = 0;
                break;
            }
        }
    }
    freeifaddrs(ifa_list);
    return found;
}

int efs_numa_for_peer(const char *host, uint16_t port, int *node_out, cpu_set_t *set,
                      char *ifname, size_t ifname_len)
{
    if (node_out)
        *node_out = -1;
    if (set)
        CPU_ZERO(set);
    if (ifname && ifname_len)
        ifname[0] = '\0';
    if (!host || !node_out || !set)
        return -1;

    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sslen) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    char local_if[IFNAMSIZ];
    if (ifname_for_local_addr((struct sockaddr *)&ss, local_if, sizeof(local_if)) != 0)
        return -1;
    if (ifname && ifname_len)
        snprintf(ifname, ifname_len, "%s", local_if);

    int node = -1;
    if (numa_from_netif(local_if, &node) != 0)
        return -1;
    *node_out = node;
    if (efs_numa_cpus_for_node(node, set) != 0) {
        *node_out = -1;
        CPU_ZERO(set);
        return -1;
    }
    return 0;
}

void efs_numa_format_cpuset(const cpu_set_t *set, char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return;
    buf[0] = '\0';
    if (!set) {
        snprintf(buf, buflen, "(none)");
        return;
    }
    size_t used = 0;
    int first = 1;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, set))
            continue;
        int start = c;
        while (c + 1 < CPU_SETSIZE && CPU_ISSET(c + 1, set))
            c++;
        int n;
        if (start == c)
            n = snprintf(buf + used, buflen - used, first ? "%d" : ",%d", start);
        else
            n = snprintf(buf + used, buflen - used, first ? "%d-%d" : ",%d-%d",
                         start, c);
        if (n < 0 || (size_t)n >= buflen - used)
            break;
        used += (size_t)n;
        first = 0;
    }
    if (first)
        snprintf(buf, buflen, "(none)");
}

void efs_numa_apply_affinity(const cpu_set_t *set)
{
    if (!set || CPU_COUNT(set) == 0)
        return;
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), set);
}

int efs_numa_affinity_enabled(void)
{
    const char *e = getenv("EFS_NUMA_AFFINITY");
    if (!e || !*e)
        return 0;
    return strcmp(e, "1") == 0 || strcmp(e, "on") == 0 ||
           strcmp(e, "true") == 0;
}
