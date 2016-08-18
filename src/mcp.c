#define MAIN

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

enum { kHugeBlockSize = 1ull << 21 };

#define ALIGN(s, a) (((s) + (a) - 1) & ~(a - 1))

int
main(int argc, char *argv[]) {
  char const * src;
  char path[PATH_MAX + 1], *dst;
  int n, fd;
  size_t i, d = argc - 1;
  struct stat src_st, dst_st;
  off_t size;

  if (argc < 3) {
    fprintf(stderr, "%s [SOURCE] [DIRECTORY]\n", argv[0]);
    return 0;
  }

  for (i = 1; i < d; ++i) {
    n = snprintf(path, PATH_MAX, "%s/%s", argv[d], basename(argv[i]));
    if (n >= PATH_MAX) {
      fprintf(stderr, "Name too long: '%s/%s'\n", argv[d], basename(argv[i]));
      return 1;
    }

    fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("Could not open source");
      return 1;
    }

    n = fstat(fd, &src_st);
    if (n) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("failed to stat source file.");
      return 1;
    }


    src = mmap(NULL, src_st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (src == MAP_FAILED) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("Could not map source");
    }
    close(fd);

    fd = open(path, O_RDWR | O_CREAT, src_st.st_mode);
    if (n < 0) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("Could not open destination");
      return 1;
    }

    n = fstat(fd, &dst_st);
    if (n) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("failed to stat destination file.");
      return 1;
    }

    size = ALIGN(src_st.st_size, dst_st.st_blksize);
    n = ftruncate(fd, size);
    if (n < 0) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      fprintf(stderr, "Attempting to set size to %x (aligned to %x)\n", size, kHugeBlockSize);
      perror("Could not truncate destination");
      return 1;
    }

    dst = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dst == MAP_FAILED) {
      fprintf(stderr, "Failed to copy '%s' to '%s'\n", argv[i], path);
      perror("Could not map destination");
      return 1;
    }
    close(fd);

    memcpy(dst, src, src_st.st_size);
    munmap((void*) src, src_st.st_size);
    munmap(dst, src_st.st_size);

  }
}
