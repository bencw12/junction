#include <stdlib.h>
#include <stdio.h>
int main(void){
  char *p = malloc(4 << 20);        /* large -> mmap'd chunk */
  p[0] = 1;
  char *q = realloc(p, 16 << 20);   /* grow it */
  q[0] = 2;
  printf("moved: %s\n", p == q ? "no" : "yes");
  free(q);
  return 0;
}
