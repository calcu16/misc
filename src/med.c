#define MAIN

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char *argv[]) {
  char * p;
  int n, fd;
  struct stat st;
  size_t offset;
  uint64_t value;
  uint64_t blk;
  uint64_t blk_offset;


  if (argc != 2) {
    fprintf(stderr, "%s [SOURCE]\n", argv[0]);
    return 0;
  }

  fd = open(argv[1], O_RDWR);
  if (fd < 0) {
    perror("Could not open source");
    return 1;
  }

  n = fstat(fd, &st);
  if (n) {
    fprintf(stderr, "Failed to state '%s'\n", argv[1]);
    perror("failed to stat source file.");
    return 1;
  }

  while ((n = scanf("%zu=%llu", &offset, &value)) != -1) {
    if (st.st_size <= offset) {
      fprintf(stderr, "out of range %zu <= %zu\n", st.st_size, offset);
    }

    blk = offset / st.st_blksize;
    blk_offset = offset % st.st_blksize;

    p = mmap(NULL, st.st_blksize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, blk * st.st_blksize);
    if (p == MAP_FAILED) {
      fprintf(stderr, "Failed to mmap in %zu bytes starting at %zu\n", st.st_blksize, blk * st.st_blksize);
      perror("Could not map source");
    }

    printf("%zu=%llx\n",offset,*((uint64_t*)(p + blk_offset)));
    if (n == 2) {
      *((uint64_t*)(p + blk_offset)) = value;
    }
  }
}
