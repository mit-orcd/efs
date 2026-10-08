#include "kv_lsm_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static struct kv_wal *wal;
static unsigned n;
static double *lat;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *worker(void *arg){unsigned id=(unsigned)(uintptr_t)arg;char value[1024]={0};struct efs_kv_item item={EFS_KV_PUT,(uint8_t*)"key",3,(uint8_t*)value,sizeof(value)};for(unsigned i=0;i<n;i++){uint64_t seq;double t=now();assert(kv_wal_append(wal,&item,1,&seq)==EFS_OK);lat[id*n+i]=(now()-t)*1e6;}return NULL;}
static int replay(void *p,uint8_t op,const uint8_t*k,uint32_t kl,const uint8_t*v,uint32_t vl){(void)op;(void)k;(void)kl;(void)v;(void)vl;(*(unsigned*)p)++;return EFS_OK;}
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return(x>y)-(x<y);}
int main(int argc,char**argv){assert(argc==3);unsigned q=atoi(argv[1]);n=atoi(argv[2]);assert(q&&q<=16&&n);char path[]="/data1/efs/w96-cost-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&wal)==EFS_OK);lat=calloc(q*n,sizeof(double));assert(lat);pthread_t threads[16];double t=now();for(unsigned i=0;i<q;i++)assert(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i));for(unsigned i=0;i<q;i++)pthread_join(threads[i],NULL);double elapsed=now()-t;kv_wal_close(wal);unsigned count=0;assert(kv_wal_replay(path,replay,&count)==EFS_OK&&count==q*n);unlink(path);qsort(lat,count,sizeof(double),cmp);printf("{\"qd\":%u,\"records\":%u,\"ops_s\":%.1f,\"p50_us\":%.1f,\"p95_us\":%.1f,\"p99_us\":%.1f}\n",q,count,count/elapsed,lat[count/2],lat[count*95/100],lat[count*99/100]);free(lat);}
