/*
 * as_demo.c - keeps several forked processes running at once so that an
 * outside observer can confirm they really do occupy separate address spaces.
 *
 * Each process writes a distinctive byte over a private page and then spins,
 * re-checking that the page still holds its own value. If two of them shared an
 * address space -- or if a scheduling core were ever bound to the wrong one --
 * the check would fail. Meanwhile scripts/as_demo.sh reads the per-thread maps
 * of the Junction host process from outside and shows that different scheduling
 * threads are looking at different memory at the same addresses.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "timing.h"

int main(int argc, char *argv[]) {
  int nkids = argc > 1 ? atoi(argv[1]) : 3;
  unsigned seconds = argc > 2 ? (unsigned)atoi(argv[2]) : 15;
  size_t len = 1 << 20;

  setvbuf(stdout, NULL, _IOLBF, 0);

  char *region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  printf("region %p len %zu\n", (void *)region, len);

  for (int i = 0; i < nkids; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 1;
    }
    if (pid == 0) {
      unsigned char tok = (unsigned char)(i + 1);
      memset(region, tok, len);
      printf("child %d pid %d token %u\n", i, getpid(), tok);
      uint64_t end = monotonic_ns() + (uint64_t)seconds * 1000000000ULL;
      unsigned long long spins = 0;
      while (monotonic_ns() < end) {
        for (size_t off = 0; off < len; off += 4093)
          if ((unsigned char)region[off] != tok) {
            printf("child %d SAW FOREIGN DATA at +%zu\n", i, off);
            _exit(1);
          }
        spins++;
      }
      printf("child %d verified %llu times\n", i, spins);
      _exit(0);
    }
  }

  memset(region, 0xff, len);
  printf("parent pid %d token 255\n", getpid());
  uint64_t end = monotonic_ns() + (uint64_t)seconds * 1000000000ULL;
  while (monotonic_ns() < end) {
    for (size_t off = 0; off < len; off += 4093)
      if ((unsigned char)region[off] != 0xff) {
        printf("parent SAW FOREIGN DATA at +%zu\n", off);
        return 1;
      }
  }

  int bad = 0;
  for (int i = 0; i < nkids; i++) {
    int st = 0;
    wait(&st);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
  }
  printf("%s\n", bad == 0 ? "PASS" : "FAIL");
  return bad == 0 ? 0 : 1;
}
