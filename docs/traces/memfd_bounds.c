/* Does the kernel keep an mmap of a memfd inside the memfd's size? */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <linux/memfd.h>

#define SZ (16UL << 20)   /* 16 MB "arena" */

static sigjmp_buf jb;
static void bus(int s) { (void)s; siglongjmp(jb, 1); }

static const char *touch(volatile char *p) {
  struct sigaction sa = {0}, old_bus, old_segv;
  sa.sa_handler = bus;
  sigaction(SIGBUS, &sa, &old_bus);
  sigaction(SIGSEGV, &sa, &old_segv);
  const char *r;
  if (sigsetjmp(jb, 1) == 0) { *p = 'x'; r = "write OK"; }
  else r = "FAULTED (SIGBUS/SIGSEGV)";
  sigaction(SIGBUS, &old_bus, NULL);
  sigaction(SIGSEGV, &old_segv, NULL);
  return r;
}

int main(void) {
  int fd = memfd_create("arena", MFD_ALLOW_SEALING);
  if (fd < 0) { perror("memfd_create"); return 1; }
  if (ftruncate(fd, SZ)) { perror("ftruncate"); return 1; }
  printf("arena: memfd sized %lu MB\n\n", SZ >> 20);

  void *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  printf("1. mmap inside  (off=0):        %s   -> %s\n",
         p == MAP_FAILED ? "FAILED" : "ok", p == MAP_FAILED ? "-" : touch(p));

  void *q = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, SZ);
  printf("2. mmap at EOF  (off=%luM):     %s   -> %s\n", SZ >> 20,
         q == MAP_FAILED ? "FAILED" : "ok", q == MAP_FAILED ? "-" : touch(q));

  void *r = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, SZ * 4);
  printf("3. mmap past EOF (off=%luM):    %s   -> %s\n", (SZ * 4) >> 20,
         r == MAP_FAILED ? "FAILED" : "ok", r == MAP_FAILED ? "-" : touch(r));

  void *s = mmap(NULL, SZ * 2, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  printf("4. mmap straddling EOF (2x len): %s\n",
         s == MAP_FAILED ? "FAILED" : "ok");
  if (s != MAP_FAILED) {
    printf("     ...first page:            %s\n", touch(s));
    printf("     ...page past EOF:         %s\n", touch((char *)s + SZ));
  }

  printf("\n5. can the size be locked?\n");
  printf("     F_SEAL_GROW|F_SEAL_SHRINK:  %s\n",
         fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == 0
             ? "sealed" : "failed");
  printf("     ftruncate bigger after seal: %s\n",
         ftruncate(fd, SZ * 2) == 0 ? "GREW (bad)" : "refused (good)");
  return 0;
}
