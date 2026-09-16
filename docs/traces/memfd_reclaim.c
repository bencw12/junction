/* Does unmapping / MADV_DONTNEED actually free memfd pages? */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/memfd.h>
#include <linux/falloc.h>

#define SZ (64UL << 20)

static int fd;
static void show(const char *what) {
  struct stat st; fstat(fd, &st);
  printf("  %-42s resident in memfd: %5llu MB\n", what,
         (unsigned long long)st.st_blocks * 512 / (1024 * 1024));
}

int main(void) {
  fd = memfd_create("a", 0);
  ftruncate(fd, SZ);
  show("after ftruncate (sparse)");

  char *p = mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  memset(p, 'x', SZ);
  show("after writing 64 MB");

  madvise(p, SZ, MADV_DONTNEED);
  show("after MADV_DONTNEED");
  printf("  %-42s first byte reads 0x%02x\n", "", (unsigned char)p[0]);

  munmap(p, SZ);
  show("after munmap");

  fallocate(fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE, 0, SZ);
  show("after FALLOC_FL_PUNCH_HOLE");

  p = mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  printf("  %-42s first byte reads 0x%02x\n", "remapped after punch", (unsigned char)p[0]);
  return 0;
}
