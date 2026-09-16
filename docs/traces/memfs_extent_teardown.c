/* Deleting a memfs file unmaps its extent -- in one address space only.
 *
 * memfs backs each file with a 256 MB memfd extent, mapped at a fixed address.
 * Closing the last reference punches the extent and unmaps it. With a forked
 * child alive, that unmap reaches the address space that performed it and no
 * other: every other address space keeps a live VMA over memory that has been
 * freed, and memfs will hand the same offset to the next file.
 *
 * Nothing here crashes today -- the child never touches those addresses. The
 * point is that the divergence is silent, which is what CheckFrozenViolation()
 * exists to say out loud. Run under --debug_as_audit to see the divergence
 * itself. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(void) {
  pid_t c = fork();
  if (c == 0) { sleep(2); _exit(0); }

  for (int i = 0; i < 3; i++) {
    char p[64];
    snprintf(p, sizeof(p), "/tmp/frozen_%d.dat", i);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    write(fd, "x", 1);
    close(fd);
    unlink(p);
  }
  printf("parent: created and deleted 3 files\n");
  fflush(stdout);

  int st;
  waitpid(c, &st, 0);
  printf("child: exited %d\n", WEXITSTATUS(st));
  return 0;
}
