#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* Provokes a LibOS-side allocation (via --debug_libos_alloc, hooked on the
 * first getpid), then forks.  The child touches its own heap: if the LibOS
 * allocation landed in this guest's address space, the fork snapshot and the
 * later free are both wrong. */
int main(void) {
  pid_t before = getpid();
  printf("guest: pid %d\n", before);
  fflush(stdout);

  char *p = malloc(1 << 20);
  memset(p, 0x5a, 1 << 20);

  pid_t c = fork();
  if (c == 0) {
    memset(p, 0xa5, 1 << 20);
    getpid();
    _exit(p[0] == (char)0xa5 ? 0 : 3);
  }
  int st = 0;
  waitpid(c, &st, 0);
  /* back in the parent, in the parent's address space */
  getpid();
  printf("guest: child status %d\n", WEXITSTATUS(st));
  return WEXITSTATUS(st);
}
