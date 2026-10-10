#include <stdlib.h>
void w93_gnu_qsort(void *,size_t,size_t,int (*)(const void *,const void *,void *),void *);
#define qsort_r w93_gnu_qsort
