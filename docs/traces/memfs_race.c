/* Does a memfs extent created AFTER a fork reach the child?
 *
 * The child is forked first, so its address space is a snapshot. The parent
 * then creates a new file, which makes Junction map a fresh 256 MB memfs
 * extent -- into the parent's address space only. The child then reads that
 * file, which memcpy's out of the extent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>


/* Junction's /tmp is memfs; the host's is not. The native comparison runs with
 * TMPDIR pointed somewhere else so it cannot leave files in the host's /tmp,
 * where Junction would then resolve them through the read-only linuxfs. */
static const char *dir(void) {
  const char *d = getenv("TMPDIR");
  return d && *d ? d : "/tmp";
}

int main(void) {
  int to_child[2], to_parent[2];
  if (pipe(to_child) || pipe(to_parent)) return 1;

  pid_t c = fork();
  if (c == 0) {
    char go;
    read(to_child[0], &go, 1);          /* wait until the file exists */
    char path[128];
    snprintf(path, sizeof(path), "%s/jx_after_fork.dat", dir());
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("child open"); _exit(2); }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) { perror("child read"); _exit(3); }
    printf("child: read %zd bytes, first=%c\n", n, buf[0]);
    fflush(stdout);
    _exit(buf[0] == 'A' ? 0 : 4);
  }

  /* parent: create the file only now, so its extent is mapped after the
   * child's address space was already cloned */
  char path[128];
  snprintf(path, sizeof(path), "%s/jx_after_fork.dat", dir());
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { perror("parent open"); return 1; }
  char *data = malloc(4096);
  memset(data, 'A', 4096);
  write(fd, data, 4096);
  close(fd);
  printf("parent: created the file\n");
  fflush(stdout);

  write(to_child[1], "g", 1);
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFSIGNALED(st))
    printf("child: KILLED by signal %d\n", WTERMSIG(st));
  else
    printf("child: exited %d\n", WEXITSTATUS(st));
  return 0;
}
