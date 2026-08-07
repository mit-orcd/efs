#include "efs/numa_locality.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

static int fail;

static void expect_ok(const char *name, int rc)
{
    if (rc != 0) {
        fprintf(stderr, "FAIL %s: rc=%d\n", name, rc);
        fail++;
    }
}

static void expect_fail(const char *name, int rc)
{
    if (rc == 0) {
        fprintf(stderr, "FAIL %s: expected error\n", name);
        fail++;
    }
}

static void expect_cpu(const char *name, const cpu_set_t *set, int cpu, int on)
{
    int is = CPU_ISSET(cpu, set);
    if ((is != 0) != (on != 0)) {
        fprintf(stderr, "FAIL %s: cpu %d expected %s\n", name, cpu,
                on ? "set" : "clear");
        fail++;
    }
}

int main(void)
{
    fail = 0;
    setlinebuf(stdout);
    setlinebuf(stderr);

    {
        cpu_set_t set;
        expect_ok("parse simple", efs_numa_parse_cpulist("0-3,8,10-11", &set));
        expect_cpu("0", &set, 0, 1);
        expect_cpu("3", &set, 3, 1);
        expect_cpu("4", &set, 4, 0);
        expect_cpu("8", &set, 8, 1);
        expect_cpu("9", &set, 9, 0);
        expect_cpu("10", &set, 10, 1);
        expect_cpu("11", &set, 11, 1);
        if (CPU_COUNT(&set) != 7) {
            fprintf(stderr, "FAIL count want 7 got %d\n", CPU_COUNT(&set));
            fail++;
        }
    }

    {
        cpu_set_t set;
        expect_ok("parse single", efs_numa_parse_cpulist("5", &set));
        expect_cpu("5", &set, 5, 1);
        if (CPU_COUNT(&set) != 1) {
            fprintf(stderr, "FAIL single count\n");
            fail++;
        }
    }

    {
        cpu_set_t set;
        expect_fail("parse empty", efs_numa_parse_cpulist("", &set));
        expect_fail("parse junk", efs_numa_parse_cpulist("abc", &set));
        expect_fail("parse bad range", efs_numa_parse_cpulist("5-3", &set));
    }

    {
        cpu_set_t proc;
        CPU_ZERO(&proc);
        if (sched_getaffinity(0, sizeof(proc), &proc) != 0) {
            fprintf(stderr, "FAIL sched_getaffinity\n");
            fail++;
        } else {
            cpu_set_t big;
            CPU_ZERO(&big);
            for (int c = 0; c < CPU_SETSIZE; c++)
                CPU_SET(c, &big);
            expect_ok("intersect", efs_numa_intersect_process_affinity(&big));
            for (int c = 0; c < CPU_SETSIZE; c++) {
                if (CPU_ISSET(c, &big) && !CPU_ISSET(c, &proc)) {
                    fprintf(stderr, "FAIL intersect leaked cpu %d\n", c);
                    fail++;
                    break;
                }
            }
            if (CPU_COUNT(&big) != CPU_COUNT(&proc)) {
                fprintf(stderr, "FAIL intersect count %d vs proc %d\n",
                        CPU_COUNT(&big), CPU_COUNT(&proc));
                fail++;
            }
        }
    }

    {
        cpu_set_t proc;
        CPU_ZERO(&proc);
        if (sched_getaffinity(0, sizeof(proc), &proc) == 0) {
            int free_cpu = -1;
            for (int c = 0; c < CPU_SETSIZE; c++) {
                if (!CPU_ISSET(c, &proc)) {
                    free_cpu = c;
                    break;
                }
            }
            if (free_cpu >= 0) {
                cpu_set_t only;
                CPU_ZERO(&only);
                CPU_SET(free_cpu, &only);
                expect_fail("intersect empty",
                            efs_numa_intersect_process_affinity(&only));
            }
        }
    }

    {
        char buf[64];
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        CPU_SET(1, &set);
        CPU_SET(2, &set);
        CPU_SET(5, &set);
        efs_numa_format_cpuset(&set, buf, sizeof(buf));
        if (strcmp(buf, "0-2,5") != 0) {
            fprintf(stderr, "FAIL format got '%s'\n", buf);
            fail++;
        }
    }

    {
        unsetenv("EFS_NUMA_AFFINITY");
        if (efs_numa_affinity_enabled() != 0) {
            fprintf(stderr, "FAIL affinity default should be off\n");
            fail++;
        }
        setenv("EFS_NUMA_AFFINITY", "1", 1);
        if (efs_numa_affinity_enabled() != 1) {
            fprintf(stderr, "FAIL affinity=1 should be on\n");
            fail++;
        }
        setenv("EFS_NUMA_AFFINITY", "on", 1);
        if (efs_numa_affinity_enabled() != 1) {
            fprintf(stderr, "FAIL affinity=on should be on\n");
            fail++;
        }
        setenv("EFS_NUMA_AFFINITY", "0", 1);
        if (efs_numa_affinity_enabled() != 0) {
            fprintf(stderr, "FAIL affinity=0 should be off\n");
            fail++;
        }
        unsetenv("EFS_NUMA_AFFINITY");
    }

    if (fail == 0)
        printf("test_numa_locality: OK\n");
    else
        printf("test_numa_locality: %d failures\n", fail);
    return fail ? 1 : 0;
}
