/* Measurement-export only; no product include or runtime setting. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
static volatile sig_atomic_t af_enabled;
static uint64_t af_hist[4][64], af_max[4];
static const char *af_names[]={"pump_work_ns","pump_period_ns","status_wait_ns","status_hold_ns"};
static uint64_t af_now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static void af_record(unsigned k,uint64_t n) {
    if(!__atomic_load_n(&af_enabled,__ATOMIC_RELAXED))return;
    unsigned b=n?63u-(unsigned)__builtin_clzll(n):0;
    __atomic_fetch_add(&af_hist[k][b],1,__ATOMIC_RELAXED);
    uint64_t old=__atomic_load_n(&af_max[k],__ATOMIC_RELAXED);
    while(n>old&&!__atomic_compare_exchange_n(&af_max[k],&old,n,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
static void af_phase(int s){(void)s;__atomic_xor_fetch(&af_enabled,1,__ATOMIC_RELAXED);}
__attribute__((constructor)) static void af_init(void){signal(SIGUSR2,af_phase);}
__attribute__((destructor)) static void af_finish(void){
    for(unsigned k=0;k<4;k++){
        uint64_t n=0;for(unsigned b=0;b<64;b++)n+=af_hist[k][b];
        if(!n)continue;
        fprintf(stderr,"af-probe metric=%s count=%llu max=%llu bins=",af_names[k],
                (unsigned long long)n,(unsigned long long)af_max[k]);
        for(unsigned b=0;b<64;b++)fprintf(stderr,"%s%llu",b?",":"",(unsigned long long)af_hist[k][b]);
        fputc('\n',stderr);
    }
}
