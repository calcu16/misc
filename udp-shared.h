#ifndef UDP_SHARED_H
#define UDP_SHARED_H
#include "traffic-shared.h"
struct request {
  uint64_t seq, response_len, request_write_start, request_sel, request_rcvd, request_read_start, request_read_end, response_write_start, response_rcvd, response_read_start, response_read_end;
};

uint64_t rcvd_microseconds(int fd);
#endif/*UDP_SHARED_H*/

