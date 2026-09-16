/* fork + exec, with the parent still running concurrently. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
  int spin = argc > 1 ? atoi(argv[1]) : 1;
  setvbuf(stdout, NULL, _IOLBF, 0);
  pid_t c = fork();
  if (c == 0) { execl("/bin/sleep", "sleep", "1", (char *)NULL); _exit(127); }
  if (spin) { volatile unsigned long x = 0; for (long i = 0; i < 400000000L; i++) x += i; }
  int st = 0; waitpid(c, &st, 0);
  printf("parent: child exited %d\n", WEXITSTATUS(st));
  return 0;
}
