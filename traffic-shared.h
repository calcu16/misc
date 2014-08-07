#ifndef TRAFFIC_SHARED_H
#define TRAFFIC_SHARED_H
#include <stdint.h>

#define LOG(log, level, msg)                                                 \
  do {                                                                       \
    if (level >= log_level) {                                                \
      printf("%lu %s: " msg, microseconds(), app_type);                      \
      if (log != NULL) {                                                     \
        fprintf(log, "%lu %s: " msg, microseconds(), app_type);              \
      }                                                                      \
    }                                                                        \
  } while(0)

#define LOGF(log, level, msg, ...)                                           \
  do {                                                                       \
    if (level >= log_level) {                                                \
      printf("%lu %s: " msg, microseconds(), app_type, __VA_ARGS__);         \
      if (log != NULL) {                                                     \
        fprintf(log, "%lu %s: " msg, microseconds(), app_type, __VA_ARGS__); \
      }                                                                      \
    }                                                                        \
  } while(0)

#define SETSOCKOPT(log, loglevel, fd, level, optname, optval)                \
  do {                                                                       \
    setintsockopt(log, loglevel, fd, level, optname, #optname, optval);      \
  } while(0)

#define LOGSOCKOPT(log, loglevel, fd, level, optname)                        \
  do {                                                                       \
    logintsockopt(log, loglevel, fd, level, optname, #optname);              \
  } while(0)



#define LOG_LEVEL_V 0
#define LOG_LEVEL_L 1
#define LOG_LEVEL_Q 2
extern char log_level;
extern char *app_type;

struct sockaddr;

struct setup_header {
  uint64_t requests, request_size, response_size, simul;
};

struct request_header {
  uint64_t seq, index;
};

struct response_header {
  uint64_t prev_seq, prev_write_end, prev_index, seq, index, rcvd, read_start, read_end, write_start;
};


struct request {
  size_t seq, index;
  uint64_t request_write_start, request_write_end, request_rcvd, request_read_start, request_read_end;
  uint64_t response_write_start, response_write_end, response_rcvd, response_read_start, response_read_end;
};


uint64_t microseconds(void);
uint64_t rcvd_microseconds(int fd);

int open_socketfd(char *hostname, char* port, int flags, int type, int (*func)(int, const struct sockaddr*, socklen_t));
void fputs2(FILE* out, char* buf, size_t n);
int fgets2(FILE* in, char* buf, size_t n);

size_t request_find_slot(struct request* requests, size_t seq, size_t expected, size_t size);

void setintsockopt(FILE* log, int loglevel, int sockfd, int level, int optname, char *optstring, int *optval);
void logintsockopt(FILE* log, int loglevel, int sockfd, int level, int optname, char *optstring);
#endif/*TRAFFIC_SHARED_H*/

