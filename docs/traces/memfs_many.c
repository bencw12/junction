/* Several memfs extents created after a fork, all read by the child.
 *
 * One file exercises one extent and one repair. This walks eight, each at its
 * own address and its own memfd offset, so a handler that repaired with a
 * constant offset -- or that only ever worked for the first slot -- fails
 * here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define N 8


/* Junction's /tmp is memfs; the host's is not. The native comparison runs with
 * TMPDIR pointed somewhere else so it cannot leave files in the host's /tmp,
 * where Junction would then resolve them through the read-only linuxfs. */
static const char *dir(void) {
  const char *d = getenv("TMPDIR");
  return d && *d ? d : "/tmp";
}

int main(void) {
  int to_child[2];
  if (pipe(to_child)) return 1;

  pid_t c = fork();
  if (c == 0) {
    char go;
    read(to_child[0], &go, 1);
    for (int i = 0; i < N; i++) {
      char path[64];
      snprintf(path, sizeof(path), "%s/many_%d.dat", dir(), i);
      int fd = open(path, O_RDONLY);
      if (fd < 0) { printf("child: open %d\n", i); _exit(2); }
      char buf[16];
      ssize_t n = read(fd, buf, sizeof(buf));
      close(fd);
      if (n <= 0 || buf[0] != 'a' + i) {
        printf("child: file %d read %c want %c\n", i, n > 0 ? buf[0] : '?',
               'a' + i);
        _exit(3);
      }
    }
    printf("child: all %d files correct\n", N);
    fflush(stdout);
    _exit(0);
  }

  for (int i = 0; i < N; i++) {
    char path[64];
    snprintf(path, sizeof(path), "%s/many_%d.dat", dir(), i);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("create"); return 1; }
    char buf[16];
    memset(buf, 'a' + i, sizeof(buf));
    write(fd, buf, sizeof(buf));
    close(fd);
  }
  write(to_child[1], "g", 1);

  int st = 0;
  waitpid(c, &st, 0);
  if (WIFSIGNALED(st)) printf("child: KILLED by signal %d\n", WTERMSIG(st));
  else printf("child: exited %d\n", WEXITSTATUS(st));
  for (int i = 0; i < N; i++) {
    char path[64];
    snprintf(path, sizeof(path), "%s/many_%d.dat", dir(), i);
    unlink(path);
  }
  return WIFSIGNALED(st) || WEXITSTATUS(st);
}
