// HIP-mapped pinned host memory + verbs registration; not GPU-direct DMA.
#include <hip/hip_runtime.h>
#include <infiniband/verbs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
__global__ void transform(unsigned *p, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) p[i] ^= 0x5a5a5a5aU;
}
#define HIP_OK(expr) do { hipError_t e=(expr); if(e!=hipSuccess) { \
 fprintf(stderr,"%s: %s\n",#expr,hipGetErrorString(e)); return 1; } } while(0)
int main() {
    int count=0; HIP_OK(hipGetDeviceCount(&count));
    if (!count) { fprintf(stderr,"SKIP: no HIP device\n"); return 77; }
    int ndev=0; ibv_device **list=ibv_get_device_list(&ndev);
    const char *wanted=getenv("EFS_RDMA_DEV"); ibv_device *device=nullptr;
    for(int i=0;i<ndev;i++) if(!wanted || !strcmp(wanted,ibv_get_device_name(list[i]))) {device=list[i];break;}
    if(!device) { fprintf(stderr,"SKIP: no requested verbs device\n"); ibv_free_device_list(list);return 77; }
    ibv_context *ctx=ibv_open_device(device);ibv_free_device_list(list);
    if(!ctx) {perror("ibv_open_device");return 1;}
    ibv_pd *pd=ibv_alloc_pd(ctx);if(!pd) {perror("ibv_alloc_pd");ibv_close_device(ctx);return 1;}
    const unsigned n=65536;void *host=nullptr,*gpu=nullptr;
    hipError_t e=hipHostMalloc(&host,n*sizeof(unsigned),hipHostMallocMapped);
    if(e!=hipSuccess) {fprintf(stderr,"hipHostMalloc: %s\n",hipGetErrorString(e));ibv_dealloc_pd(pd);ibv_close_device(ctx);return 1;}
    ibv_mr *mr=ibv_reg_mr(pd,host,n*sizeof(unsigned),IBV_ACCESS_LOCAL_WRITE);
    if(!mr) {perror("ibv_reg_mr");hipHostFree(host);ibv_dealloc_pd(pd);ibv_close_device(ctx);return 1;}
    int rc=0;e=hipHostGetDevicePointer(&gpu,host,0);
    if(e==hipSuccess) {
        for(unsigned i=0;i<n;i++) static_cast<unsigned*>(host)[i]=i;
        hipLaunchKernelGGL(transform,dim3((n+255)/256),dim3(256),0,0,static_cast<unsigned*>(gpu),n);
        e=hipGetLastError();if(e==hipSuccess)e=hipDeviceSynchronize();
        if(e==hipSuccess)for(unsigned i=0;i<n;i++)if(static_cast<unsigned*>(host)[i]!=(i^0x5a5a5a5aU)){rc=1;break;}
    }
    if(e!=hipSuccess){fprintf(stderr,"HIP: %s\n",hipGetErrorString(e));rc=1;}
    if(ibv_dereg_mr(mr))rc=1;
    if(hipHostFree(host)!=hipSuccess)rc=1;
    if(ibv_dealloc_pd(pd))rc=1;
    if(ibv_close_device(ctx))rc=1;
    if(!rc)puts("PASS: registered mapped pinned host memory; GPU transform verified (not GPU-direct DMA)");
    return rc;
}
