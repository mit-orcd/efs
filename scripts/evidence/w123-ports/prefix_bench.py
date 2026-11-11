#!/usr/bin/env python3
"""Time key_pfx of two kv_lsm.c sources (old, new): 4 million calls per key length, 5 trials.

usage: prefix_bench.py OLD/src/kv/kv_lsm.c NEW/src/kv/kv_lsm.c
Prints the median milliseconds per arm; the checksum column must be equal
(same prefix values). No daemon, one core.
"""
import subprocess, sys, tempfile
from pathlib import Path

def body(path, name):
    s = Path(path).read_text()
    return s[s.index('static __uint128_t key_pfx'):s.index('/* Index of key')].replace('key_pfx', name, 1)

code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
''' + body(sys.argv[1], 'pfx_old') + body(sys.argv[2], 'pfx_new') + r'''
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#define N 4000000u
#define ARM(fn, len) do { double t[5]; uint64_t sum = 0; \
    for (int r = 0; r < 5; r++) { double a = ms(); sum = 0; \
        for (uint32_t i = 0; i < N; i++) { __uint128_t v = fn(keys + (i & 4095u) * 16u, len); sum += (uint64_t)v ^ (uint64_t)(v >> 64); } \
        t[r] = ms() - a; } \
    qsort(t, 5, sizeof(t[0]), cmp); printf("%-8s len=%-2u median %.1f ms  checksum %016llx\n", #fn, len, t[2], (unsigned long long)sum); } while (0)
int main(void) {
    static uint8_t keys[4096 * 16]; uint32_t state = 17;
    for (unsigned i = 0; i < sizeof(keys); i++) { state = state * 1664525u + 1013904223u; keys[i] = state >> 24; }
    ARM(pfx_old, 11u); ARM(pfx_new, 11u); ARM(pfx_old, 16u); ARM(pfx_new, 16u);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'bench.c'
    c.write_text(code)
    subprocess.run(['cc', '-O2', '-g', str(c), '-o', str(Path(d) / 'bench')], check=True)
    subprocess.run([str(Path(d) / 'bench')], check=True)
