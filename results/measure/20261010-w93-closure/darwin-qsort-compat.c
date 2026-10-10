#include <stdlib.h>
struct w93_sort {int (*cmp)(const void *,const void *,void *);void *ctx;};
static int compare(void *ctx,const void *a,const void *b){struct w93_sort *c=ctx;return c->cmp(a,b,c->ctx);}
void w93_gnu_qsort(void *a,size_t n,size_t width,int (*cmp)(const void *,const void *,void *),void *ctx){struct w93_sort c={cmp,ctx};qsort_r(a,n,width,&c,compare);}
