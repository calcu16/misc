#ifndef TRAFFIC_SHARED_H
#define TRAFFIC_SHARED_H
#include <stdint.h>

typedef struct _IO_FILE FILE;
struct sockaddr;

struct setup_header {
  uint64_t requests;
  uint64_t request_size;
  uint64_t response_size;
};

struct request_header {
  uint64_t seq;
};

struct response_header {
  uint64_t prev_seq, prev_write_end, seq, read_start, read_end, write_start;
};

uint64_t microseconds(void);

int open_socketfd(char *hostname, char* port, int flags, int (*func)(int, const struct sockaddr*, socklen_t));
void fputs2(FILE* out, char* buf, size_t n);
int fgets2(FILE* in, char* buf, size_t n);
#endif/*TRAFFIC_SHARED_H*/

