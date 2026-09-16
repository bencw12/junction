/* Does a recycled memfs slot stay correct across address spaces?
 *
 * memfs extents are now placed at kMemFsBase + offset, so a deleted file's
 * address is handed straight to the next file. A child that materialised the
 * old extent has a mapping at that address; the parent then deletes the file
 * and creates another, which lands at the same address with the same backing.
 *
 * The child must read the new file's contents, not the old file's, and must
 * not read zeros. Sequenced with pipes so the order is not a race. */
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

static int read_first(const char *path, char *out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n <= 0) return -1;
  *out = buf[0];
  return 0;
}

static void put(const char *path, char c) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { perror("create"); exit(1); }
  char *buf = malloc(4096);
  memset(buf, c, 4096);
  if (write(fd, buf, 4096) != 4096) { perror("write"); exit(1); }
  close(fd);
  free(buf);
}

int main(void) {
  char A_PATH[128], B_PATH[128];
  snprintf(A_PATH, sizeof(A_PATH), "%s/reuse_a.dat", dir());
  snprintf(B_PATH, sizeof(B_PATH), "%s/reuse_b.dat", dir());
  int to_child[2], to_parent[2];
  if (pipe(to_child) || pipe(to_parent)) return 1;

  put(A_PATH, 'A');

  pid_t c = fork();
  if (c == 0) {
    char go, got;
    /* materialise the first file's extent in this address space */
    read(to_child[0], &go, 1);
    if (read_first(A_PATH, &got) || got != 'A') {
      printf("child: first read wrong (%d)\n", got);
      _exit(2);
    }
    write(to_parent[1], "d", 1);

    /* the parent has now deleted it and created a second file, which lands
     * on the same slot -- and therefore at the same address */
    read(to_child[0], &go, 1);
    if (read_first(B_PATH, &got)) { printf("child: open b\n"); _exit(3); }
    printf("child: second read = %c\n", got);
    fflush(stdout);
    _exit(got == 'B' ? 0 : 4);
  }

  char ack;
  write(to_child[1], "g", 1);
  read(to_parent[0], &ack, 1);

  unlink(A_PATH);
  put(B_PATH, 'B');
  write(to_child[1], "g", 1);

  int st = 0;
  waitpid(c, &st, 0);
  if (WIFSIGNALED(st)) printf("child: KILLED by signal %d\n", WTERMSIG(st));
  else printf("child: exited %d\n", WEXITSTATUS(st));
  unlink(B_PATH);
  return WIFSIGNALED(st) || WEXITSTATUS(st);
}
